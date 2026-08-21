#include "sim/tts/piper_tts.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "crow/json.h"

#ifdef SIM_HAVE_PIPER
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace sim {

namespace {

// piper reads the voice sample rate out of <model>.onnx.json under
// audio.sample_rate and falls back to 22050 when the key is absent. Doing the
// same here is what keeps sample_rate() honest: the browser builds its playback
// AudioBuffer from whatever this reports, so a wrong value plays the examiner
// back at the wrong pitch and speed.
int read_voice_sample_rate(const std::string& model_path) {
    constexpr int kPiperDefaultRate = 22050;
    if (model_path.empty()) {
        return kPiperDefaultRate;
    }

    std::ifstream file(model_path + ".json");
    if (!file) {
        return kPiperDefaultRate;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    const crow::json::rvalue parsed = crow::json::load(buffer.str());
    if (!parsed || parsed.t() != crow::json::type::Object ||
        !parsed.has("audio")) {
        return kPiperDefaultRate;
    }

    const crow::json::rvalue& audio = parsed["audio"];
    if (audio.t() != crow::json::type::Object || !audio.has("sample_rate")) {
        return kPiperDefaultRate;
    }

    const crow::json::rvalue& rate = audio["sample_rate"];
    if (rate.t() != crow::json::type::Number) {
        return kPiperDefaultRate;
    }
    // Every step is checked because crow rvalue::operator[] throws on a missing
    // key and .i() throws on a wrong type, and this runs from the constructor.
    // A throw here would take down startup over a cosmetic field.

    const int value = static_cast<int>(rate.i());
    return value > 0 ? value : kPiperDefaultRate;
}

#if defined(SIM_HAVE_PIPER) && !defined(_WIN32)
std::string run_piper(const std::string& model_path, const std::string& text) {
    int in_pipe[2];   // parent writes text -> child stdin
    int out_pipe[2];  // child writes PCM   -> parent reads
    if (pipe(in_pipe) != 0) {
        throw std::runtime_error("piper: pipe() failed");
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        throw std::runtime_error("piper: pipe() failed");
        // The two pipe() calls are no longer short-circuited into one
        // condition: pipe(a) != 0 || pipe(b) != 0 leaked the descriptors of a
        // whenever b was the call that failed.
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        throw std::runtime_error("piper: fork() failed");
    }

    if (pid == 0) {
        // Child. Rewire stdin/stdout to the pipes, then exec piper.
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        // Close every original descriptor; the dup2 copies are what matter now.
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(SIM_PIPER_EXECUTABLE, SIM_PIPER_EXECUTABLE,
              "--model", model_path.c_str(),
              "--output_raw", static_cast<char*>(nullptr));
        // --model is not optional. Without it piper prints its usage to stderr
        // and exits, so the parent read below returned an empty buffer and
        // every turn arrived at the browser silent.
        _exit(127);  // exec only returns on failure
    }

    // Parent. Close the child ends so reads see EOF when piper exits.
    close(in_pipe[0]);
    close(out_pipe[1]);

    // Write the whole text, then close stdin so piper knows input is done.
    if (::write(in_pipe[1], text.data(), text.size()) < 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        throw std::runtime_error("piper: write to stdin failed");
        // The child is reaped before the throw. Without it a failed write left
        // a zombie per turn for the lifetime of the process.
    }
    close(in_pipe[1]);

    // Drain stdout to EOF.
    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(out_pipe[0], buf, sizeof(buf))) > 0) {
        raw.append(buf, static_cast<std::size_t>(n));
    }
    close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);  // reap the child; avoids a zombie
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("piper exited with a non-zero status");
        // status was read and then discarded, so a piper that died on a bad
        // model path was indistinguishable from a voice that said nothing.
    }
    return raw;
}
#endif

#if defined(SIM_HAVE_PIPER) && defined(_WIN32)
std::string run_piper(const std::string& model_path, const std::string& text) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;  // child inherits the pipe ends

    HANDLE in_read = nullptr, in_write = nullptr;
    HANDLE out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
        throw std::runtime_error("piper: CreatePipe failed");
    }
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        throw std::runtime_error("piper: CreatePipe failed");
    }
    // The parent own ends must NOT be inheritable, or the child holds a copy
    // and our read never sees EOF (the Windows analogue of the POSIX close).
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::string cmd = std::string("\"") + SIM_PIPER_EXECUTABLE +
                      "\" --model \"" + model_path + "\" --output_raw";
    // Both paths routinely contain spaces on Windows, so both are quoted.
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(in_read);  CloseHandle(in_write);
        CloseHandle(out_read); CloseHandle(out_write);
        throw std::runtime_error("piper: CreateProcess failed");
        // All four handles were leaked on this path before.
    }
    // Close the child ends in the parent - same EOF reasoning as POSIX.
    CloseHandle(in_read);
    CloseHandle(out_write);

    DWORD written = 0;
    WriteFile(in_write, text.data(), static_cast<DWORD>(text.size()), &written,
              nullptr);
    CloseHandle(in_write);  // signals end-of-input to piper

    std::string raw;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(out_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
        raw.append(buf, n);
    }
    CloseHandle(out_read);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code != 0) {
        throw std::runtime_error("piper exited with a non-zero status");
    }
    return raw;
}
#endif

// piper --output_raw writes little-endian int16 samples with no container at
// all. The previous pcm_from_wav() skipped a 44-byte RIFF header that is never
// present on this path, so the first 22 samples of every utterance were thrown
// away.
std::vector<std::int16_t> pcm_from_raw(const std::string& raw) {
    const std::size_t count = raw.size() / sizeof(std::int16_t);
    std::vector<std::int16_t> samples(count);
    if (count > 0) {
        std::memcpy(samples.data(), raw.data(), count * sizeof(std::int16_t));
        // memcpy rather than a reinterpret_cast: raw.data() is a char* with no
        // 2-byte alignment guarantee. A trailing odd byte is dropped, which is
        // correct - it cannot be half of a sample piper meant to emit.
    }
    return samples;
}

}  // namespace

PiperTTS::PiperTTS(std::string model_path)
    : model_path_(std::move(model_path)),
      sample_rate_(read_voice_sample_rate(model_path_)) {
    // model_path_ is initialised first because it is declared first, so reading
    // it here is defined. Passing the parameter instead would read a moved-from
    // string.
}

int PiperTTS::sample_rate() const {
    return sample_rate_;
}

std::vector<std::int16_t> PiperTTS::synthesize(const std::string& text) {
#ifdef SIM_HAVE_PIPER
    if (text.empty()) {
        return {};
        // piper on empty stdin produces nothing and exits, so skip the process.
    }
    return pcm_from_raw(run_piper(model_path_, text));
#else
    (void)text;
    return {};
    // No piper binary was configured. An empty result is already the "no audio"
    // signal the whole send path is built around: send_examiner_result omits
    // sample_rate, skips send_binary, and the client arms the mic immediately.
    // The old #else branch had no statements at all, so the function fell off
    // its end without returning.
#endif
}

}  // namespace sim
