# speaking-sim



Italian Exam API
Project number: 105421562997

A local speaking-exam simulator for beginner Italian. The browser captures
microphone audio and streams it over a WebSocket to a C++ server, which runs it
through speech-to-text, sends the transcript plus the conversation history to an
examiner LLM over HTTP, synthesises the reply to speech, and streams the audio
back to the browser to play.

Everything runs on your own machine apart from the examiner call, which goes
either to the Gemini API over HTTPS or to a local Ollama-compatible server over
plain HTTP.

## Status

The pipeline is wired end to end, but the four pluggable pieces are stubs: they
log `not implemented` and return placeholder values.

| Piece | File | Current behaviour |
| --- | --- | --- |
| Whisper STT | `src/stt/whisper_stt.cpp` | returns `"placeholder transcript"` |
| Gemini examiner | `src/examiner/gemini_examiner.cpp` | returns `"placeholder examiner question"` |
| Hailo examiner | `src/examiner/hailo_examiner.cpp` | returns `"placeholder examiner question"` |
| Piper TTS | `src/tts/piper_tts.cpp` | returns an empty audio buffer |

So the server starts, accepts a WebSocket connection, buffers microphone audio,
and pushes a placeholder reply back through the worker pool — no real inference
happens yet.

## Prerequisites

- CMake 3.16 or newer
- A C++17 compiler (GCC 9+, Clang 10+, or MSVC 19.2+)
- OpenSSL development headers and libraries
  - Debian/Ubuntu: `sudo apt install libssl-dev`
  - macOS: `brew install openssl@3`
  - If CMake cannot find it, pass `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`
- git, for the `third_party/` submodules

No Hailo hardware or Hailo SDK is needed. The Hailo examiner is only an HTTP
client pointed at a local Ollama-compatible server, and nothing in the build
links a Hailo SDK.

## Getting the source

```sh
git clone --recurse-submodules <repo-url>
cd Italian-Revision-App/speaking-sim
```

If you already cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

Two submodules live under `third_party/`:

| Submodule | Upstream | Role |
| --- | --- | --- |
| `whisper.cpp` | `ggml-org/whisper.cpp` (MIT) | Linked into the server as the `whisper` library |
| `piper` | `rhasspy/piper` (MIT) | Built as a standalone binary and driven as a subprocess |

Both are optional at configure time. When they are missing CMake prints a note
and configures the server without them, so a fresh clone still builds.

### A note on piper

piper publishes no C++ library — upstream defines only `add_executable(piper …)`
— so there is nothing to link against and the TTS backend has to invoke the
binary. CMake passes its path to the source as `SIM_PIPER_EXECUTABLE`.

It is deliberately left out of the default build, because building it downloads
onnxruntime, fmt and spdlog through `ExternalProject`. Build it when you need it:

```sh
cmake --build build --target piper
```

`rhasspy/piper` is archived upstream. Its successor, `OHF-Voice/piper1-gpl`, is
actively maintained but is GPL-3.0 and Python-first, so adopting it would place
this project under the GPL. The archived MIT release is pinned here instead; it
still matches the ONNX voice models `PIPER_MODEL_PATH` points at.

## Building

Out-of-source, from this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Crow, standalone Asio and cpp-httplib are downloaded by CMake's FetchContent on
the first configure and cached under `build/_deps/`, so the first run needs
network access.

The binary lands at `build/speaking-sim`. On Windows the whisper and ggml DLLs
are copied next to it after linking, since Windows has no rpath and only
searches the executable's own directory.

## Running

```sh
cp .env.example .env      # then fill in GEMINI_API_KEY and the model paths
set -a && . ./.env && set +a
./build/speaking-sim
```

**Run the binary from this directory.** It opens `web(frontend)/index.html` and
`prompts/examiner_system.txt` by relative path, so starting it from anywhere
else serves a 404 for the page and silently falls back to a built-in one-line
system prompt.

The server reads its settings from the process environment, not from `.env`
directly — hence the `set -a && . ./.env && set +a` above, which exports every
variable in the file into your shell.

Then open <http://localhost:8080> and allow microphone access.

| Variable | Default | Purpose |
| --- | --- | --- |
| `GEMINI_API_KEY` | *(empty)* | API key for the Gemini examiner |
| `EXAMINER_BACKEND` | `gemini` | `gemini` or `hailo`; anything but `hailo` means `gemini` |
| `HAILO_OLLAMA_URL` | `http://localhost:11434` | Local Ollama-compatible server for the Hailo examiner |
| `WHISPER_MODEL_PATH` | *(empty)* | whisper.cpp GGML model |
| `PIPER_MODEL_PATH` | *(empty)* | piper ONNX voice model |
| `PORT` | `8080` | Listening port |

## Layout

```
include/sim/      Public headers. The interfaces (InterfaceSTT, InterfaceExaminer,
                  InterfaceTTS) sit at the top level; concrete backends live in
                  the matching subdirectory.
src/              Implementation, mirroring include/sim/. main.cpp picks the
                  examiner backend from config and injects the concrete pieces
                  into Server as interfaces.
web(frontend)/    Browser client: index.html and client.js (mic capture, PCM
                  conversion, WebSocket, playback).
prompts/          examiner_system.txt, the examiner's system prompt, read once
                  at startup.
third_party/      git submodules: whisper.cpp and piper.
models/           Model weights. Ignored by git apart from .gitkeep — download
                  the whisper and piper models here yourself.
```

## Architecture

`main.cpp` reads the config, constructs the concrete STT/examiner/TTS objects and
hands them to `Server` as interface pointers — the backend choice is made in
exactly one place.

`Server` owns a Crow app with two routes: `GET /` serves the page, and `/ws` is
the WebSocket. Binary frames are appended to that connection's `Session` audio
buffer. A text frame of `{"type":"stop"}` drains the buffer and hands it to a
`WorkerPool` job, which runs STT, the examiner call and TTS off the socket
thread, then writes the reply text and PCM audio back to the connection.
