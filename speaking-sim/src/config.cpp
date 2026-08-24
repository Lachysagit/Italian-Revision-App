#include "sim/config.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

namespace sim {

namespace {

std::string get_env(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

bool parse_int_strict(const std::string& text, int& out) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();

    int value = 0;
    const auto result = std::from_chars(begin, end, value);

    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    //std::stoi parses the longest valid prefix, so "80abc" came back as 80.
    //from_chars reports where it stopped, and overflow through ec

    out = value;
    return true;
}

//an explicit ceiling. the pool spawns one thread per count, so an unbounded
//value taken straight from the environment is a startup-time foot-gun
constexpr int kMaxWorkerThreads = 64;

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
    int port_value = 0;
    if (!parse_int_strict(port_text, port_value)) {
        std::cerr << "PORT " << port_text << " is not a number, using 8080\n";
    } else if (port_value < 1 || port_value > 65535) {
        std::cerr << "PORT " << port_text
                  << " is outside 1-65535, using 8080\n";
        //a bare static_cast would silently wrap, so 70000 would become 4464
    } else {
        config.port = static_cast<std::uint16_t>(port_value);
    }

    const std::string threads_text = get_env("WORKER_THREADS", "0");
    config.worker_threads = 0;
    int thread_value = 0;
    if (!parse_int_strict(threads_text, thread_value)) {
        std::cerr << "WORKER_THREADS " << threads_text
                  << " is not a number, deriving from the CPU count\n";
    } else if (thread_value > kMaxWorkerThreads) {
        std::cerr << "WORKER_THREADS " << threads_text << " is above the "
                  << kMaxWorkerThreads << " cap, using " << kMaxWorkerThreads
                  << "\n";
        config.worker_threads = static_cast<std::size_t>(kMaxWorkerThreads);
        //clamped rather than rejected: the caller asked for more parallelism,
        //so the closest we can honestly give is the ceiling, not the CPU count
    } else if (thread_value > 0) {
        config.worker_threads = static_cast<std::size_t>(thread_value);
    }
    //0 and negatives fall through to the hardware default below, so
    //"WORKER_THREADS=0" means "decide for me" rather than "run no workers"

    if (config.worker_threads == 0) {
        const unsigned int cores = std::thread::hardware_concurrency();
        config.worker_threads = (cores == 0) ? 2u : cores;
        //hardware_concurrency() is allowed to return 0 when it cannot tell, so
        //the old hardcoded 2 stays as the floor rather than the ceiling

        config.worker_threads = std::min(config.worker_threads,
                                         static_cast<std::size_t>(kMaxWorkerThreads));
        //the ceiling has to apply here too: capping only the explicit value
        //would let a host with more cores walk straight past it
    }

    if (config.examiner_backend == ExaminerBackend::Gemini &&
        config.gemini_api_key.empty()) {
        std::cerr << "EXAMINER_BACKEND is gemini but GEMINI_API_KEY is empty\n";
        //the examiner will fail on its first call, so say so at startup
    }

    return config;
}

}  // namespace sim
