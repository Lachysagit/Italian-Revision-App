#include "sim/config.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
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
    config.port = 8080;
    try {
        const int port_value = std::stoi(port_text);
        //std::stoi throws on anything that does not start with a number,
        //which would otherwise abort the process before the server starts
        if (port_value >= 1 && port_value <= 65535) {
            config.port = static_cast<std::uint16_t>(port_value);
        } else {
            std::cerr << "PORT " << port_text
                      << " is outside 1-65535, using 8080\n";
            //a bare static_cast would silently wrap, so 70000 would become 4464
        }
    } catch (const std::exception&) {
        std::cerr << "PORT " << port_text << " is not a number, using 8080\n";
    }

    if (config.examiner_backend == ExaminerBackend::Gemini &&
        config.gemini_api_key.empty()) {
        std::cerr << "EXAMINER_BACKEND is gemini but GEMINI_API_KEY is empty\n";
        //the examiner will fail on its first call, so say so at startup
    }

    return config;
}

}  // namespace sim