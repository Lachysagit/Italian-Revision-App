#pragma once

#include <cstdint>
#include <string>

namespace sim {

enum class ExaminerBackend {
    Gemini,
    Hailo,
};

struct Config {
    std::string gemini_api_key;
    ExaminerBackend examiner_backend = ExaminerBackend::Gemini;
    std::string hailo_ollama_url;
    std::string whisper_model_path;
    std::string piper_model_path;
    std::uint16_t port = 8080;
};

    Config load_config();   

}  // namespace sim