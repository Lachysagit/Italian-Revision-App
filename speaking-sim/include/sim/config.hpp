#pragma once

#include <cstddef>
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

    std::size_t worker_threads = 2;
    //size of the pipeline pool. A turn holds its worker for the whole
    //STT -> examiner -> TTS chain, so this is the number of students who can be
    //mid-turn at once. Every thread past that count queues behind these
};

Config load_config();

}  // namespace sim
