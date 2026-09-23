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

//"name=value" pairs separated by commas, the shape GEMINI_API_KEYS uses and
//LANGUAGE_VOICES copies. A pair missing "=" or with an empty name/value is
//skipped rather than failing startup, since one bad entry should not take the
//rest down. Splitting on the FIRST "=" only: values carry their own, and a
//path or a key truncated at one would be worse than useless
std::vector<std::pair<std::string, std::string>> parse_named_pairs(
    const std::string& text) {
    std::vector<std::pair<std::string, std::string>> pairs;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t comma = text.find(',', start);
        const std::string pair = text.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);

        const std::size_t equals = pair.find('=');
        if (equals != std::string::npos) {
            std::string name = pair.substr(0, equals);
            std::string value = pair.substr(equals + 1);
            if (!name.empty() && !value.empty()) {
                pairs.emplace_back(std::move(name), std::move(value));
            }
        }

        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return pairs;
}

//GEMINI_API_KEYS="Personal=AIza...,Backup=AIza..." lets a settings picker
//choose which key a session's requests are billed against.
std::vector<GeminiKeyOption> parse_gemini_keys(const std::string& text) {
    std::vector<GeminiKeyOption> options;
    for (auto& [name, key] : parse_named_pairs(text)) {
        options.push_back({std::move(name), std::move(key)});
    }
    return options;
}

}  // namespace

Config load_config() {
    Config config;

    config.gemini_api_key = get_env("GEMINI_API_KEY", "");
    config.gemini_api_keys = parse_gemini_keys(get_env("GEMINI_API_KEYS", ""));
    if (config.gemini_api_keys.empty() && !config.gemini_api_key.empty()) {
        config.gemini_api_keys.push_back({"Default", config.gemini_api_key});
        //no named pool configured, fall back to the single legacy key so the
        //picker still has exactly one option rather than an empty list
    }
    if (!config.gemini_api_keys.empty()) {
        config.gemini_api_key = config.gemini_api_keys.front().key;
        //keep the singular field in step: it is what main.cpp hands to the
        //constructor default, so it must name a key that is actually in the pool
    }
    config.translate_api_key = get_env("TRANSLATE_API_KEY", "");
    //empty is allowed: the translate box is optional, and /api/translate says so
    //itself rather than the server refusing to start over a side feature

    config.hailo_ollama_url = get_env("HAILO_OLLAMA_URL", "http://localhost:11434");
    config.whisper_model_path = get_env("WHISPER_MODEL_PATH", "");
    config.piper_model_path = get_env("PIPER_MODEL_PATH", "");
    config.language_voices = parse_named_pairs(get_env("LANGUAGE_VOICES", ""));
    //LANGUAGE_VOICES="italian=models/it_IT-...onnx,german=models/de_DE-...onnx"
    //overrides the voice a language ships with. Empty is the normal case: the
    //built-in paths in language.cpp already name the voices in models/, and
    //PIPER_MODEL_PATH still overrides italian's on its own

    config.database_path = get_env("DATABASE_PATH", "speaking-sim.db");

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
