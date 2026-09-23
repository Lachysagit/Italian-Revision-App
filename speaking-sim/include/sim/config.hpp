#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sim {

enum class ExaminerBackend {
    Gemini,
    Hailo,
};

struct GeminiKeyOption {
    std::string name;
    std::string key;
};

enum class AudioInput {
    Gemini,
    //the captured audio goes to the examiner, which transcribes and replies in
    //one call. Whisper stays loaded and takes over if that call fails
    Whisper,
};

enum class AudioCodec {
    Flac,
    Wav,
    //both are lossless and carry the same samples. Wav is the buffer plus a
    //44-byte header and needs no encoder; flac halves the bytes on the wire
};

struct Config {
    std::string gemini_api_key;
    //first entry of gemini_api_keys, kept for callers that only want a default
    std::vector<GeminiKeyOption> gemini_api_keys;
    //named keys parsed from GEMINI_API_KEYS, the pool a settings picker chooses from
    std::string gemini_model;
    //the model id. Configurable because it has moved three times (3.5, 3.6,
    //3.8) and each move used to be a recompile
    std::string gemini_thinking_level;
    std::string gemini_opening_thinking_level;
    //low, medium or high. 3.8 rejects the "minimal" that 3.5 took, so an
    //unchecked value here is a 400 on every turn - validated at startup
    ExaminerBackend examiner_backend = ExaminerBackend::Gemini;
    AudioInput audio_input = AudioInput::Gemini;
    AudioCodec audio_codec = AudioCodec::Flac;
    std::string translate_api_key;
    //Google Cloud key for the translate box, separate from the Gemini pool so
    //casual lookups do not spend the examiner's rationed daily requests
    std::string hailo_ollama_url;
    std::string whisper_model_path;
    std::string piper_model_path;
    //the italian voice, kept singular so an .env written before multi-language
    //still selects it. LANGUAGE_VOICES overrides per language on top
    std::vector<std::pair<std::string, std::string>> language_voices;
    //language id -> piper voice path, parsed from LANGUAGE_VOICES. Empty means
    //every language keeps the voice built into language.cpp
    std::string database_path;
    //accounts, classes and exam history. A relative path resolves against the
    //working directory, which is speaking-sim/ like every other asset path here

    std::uint16_t port = 8080;

    std::size_t worker_threads = 2;
 
};

Config load_config();

}  // namespace sim
