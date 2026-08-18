#include "sim/config.hpp"

#include <cstdlib>
#include <string>

namespace sim {

namespace {

std::string get_env(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

}  // namespace

Config load_config() {
    Config config;

    config.gemini_api_key = get_env("GEMINI_API_KEY", "");
    config.hailo_ollama_url = get_env("HAILO_OLLAMA_URL", "http://localhost:11434");
    config.whisper_model_path = get_env("WHISPER_MODEL_PATH", "");
    config.piper_model_path = get_env("PIPER_MODEL_PATH", "");

    const std::string backend = get_env("EXAMINER_BACKEND", "gemini");
    config.examiner_backend =
        (backend == "hailo") ? ExaminerBackend::Hailo : ExaminerBackend::Gemini;

    const std::string port_text = get_env("PORT", "8080");
    config.port = static_cast<std::uint16_t>(std::stoi(port_text));

    return config;
}

}  // namespace sim