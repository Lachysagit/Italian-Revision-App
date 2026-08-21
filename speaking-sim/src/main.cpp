#include <exception>
#include <iostream>
#include <memory>

#include "sim/config.hpp"
#include "sim/server.hpp"

#include "sim/examiner/gemini_examiner.hpp"
#include "sim/examiner/hailo_examiner.hpp"
#include "sim/stt/whisper_stt.hpp"
#include "sim/tts/piper_tts.hpp"


int main() {
    try {
        sim::Config config = sim::load_config();
        //read all settings from environment variables once at startup

        auto stt = std::make_unique<sim::WhisperSTT>(config.whisper_model_path);
        auto tts = std::make_unique<sim::PiperTTS>(config.piper_model_path);
        //build the concrete STT and TTS implementations

        std::unique_ptr<sim::InterfaceExaminer> examiner;
        if (config.examiner_backend == sim::ExaminerBackend::Hailo) {
            examiner = std::make_unique<sim::HailoExaminer>(config.hailo_ollama_url);
        } else {
            examiner = std::make_unique<sim::GeminiExaminer>(config.gemini_api_key);
        }
        //choose the examiner backend based on config - the ONLY place this is decided

        sim::Server server(std::move(config),
                           std::move(stt),
                           std::move(examiner),
                           std::move(tts));
        //inject the concrete pieces into the server as interfaces

        server.run();
        //start the server - blocks here until shutdown

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "fatal: unknown error" << std::endl;
        return 1;
    }
    //without this, an exception out of load_config(), a missing whisper or piper
    //model, or a port already in use leaves main by way of std::terminate, which
    //prints nothing useful and returns no exit code worth acting on. everything
    //the server owns is still destroyed here because the throw unwinds normally
}
