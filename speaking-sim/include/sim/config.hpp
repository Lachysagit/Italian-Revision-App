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

struct Config {
    std::string gemini_api_key;
    //first entry of gemini_api_keys, kept for callers that only want a default
    std::vector<GeminiKeyOption> gemini_api_keys;
    //named keys parsed from GEMINI_API_KEYS, the pool a settings picker chooses from
    ExaminerBackend examiner_backend = ExaminerBackend::Gemini;
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
    std::uint16_t port = 8080;

    std::size_t worker_threads = 2;
 
};

Config load_config();

}  // namespace sim
