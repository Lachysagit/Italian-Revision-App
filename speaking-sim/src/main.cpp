#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "sim/audio_encode.hpp"
#include "sim/config.hpp"
#include "sim/server.hpp"
#include "sim/store.hpp"

#include "sim/examiner/gemini_examiner.hpp"
#include "sim/examiner/hailo_examiner.hpp"
#include "sim/stt/whisper_stt.hpp"
#include "sim/tts/piper_tts.hpp"

namespace {

// --check-audio-encode: encode a known tone and report what came out, then
// exit. The encoders feed the examiner, and audio that decodes wrongly does
// not fail - it transcribes into plausible nonsense that reads like a bad
// answer. This runs the same code the server does, on the machine it is
// deployed to, without needing a key, a microphone or a student.
int check_audio_encode() {
    constexpr int kRate = 16000;
    std::vector<std::int16_t> samples;
    samples.reserve(kRate * 2);
    for (int i = 0; i < kRate * 2; ++i) {
        samples.push_back(static_cast<std::int16_t>(
            12000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / kRate)));
    }
    //two seconds of 440Hz: a decoder that misreads the rate or the sample
    //width produces a measurably wrong pitch rather than silence

    const sim::EncodedAudio wav = sim::encode_wav(samples, kRate);
    const sim::EncodedAudio flac = sim::encode_flac(samples, kRate);

    const std::size_t raw_bytes = samples.size() * sizeof(std::int16_t);
    std::cout << "raw pcm     " << raw_bytes << " bytes\n"
              << "wav         " << wav.bytes.size() << " bytes, "
              << wav.mime_type << "\n"
              << "flac        " << flac.bytes.size() << " bytes, "
              << flac.mime_type << " ("
              << (100 * flac.bytes.size() / wav.bytes.size())
              << "% of wav)\n"
              << "base64 flac " << sim::base64_encode(flac.bytes).size()
              << " bytes on the wire\n";

    bool ok = true;
    const auto expect = [&ok](bool condition, const char* what) {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
        ok = ok && condition;
    };

    expect(wav.bytes.size() == raw_bytes + 44, "wav is the samples plus a 44-byte header");
    expect(wav.bytes.compare(0, 4, "RIFF") == 0, "wav starts RIFF");
    expect(wav.bytes.compare(8, 4, "WAVE") == 0, "wav declares WAVE");
    expect(wav.bytes.compare(12, 4, "fmt ") == 0, "wav carries a fmt chunk");
    expect(wav.bytes.compare(36, 4, "data") == 0, "wav carries a data chunk");
    expect(static_cast<std::uint8_t>(wav.bytes[24]) == 0x80 &&
               static_cast<std::uint8_t>(wav.bytes[25]) == 0x3E,
           "wav header says 16000 Hz");
    expect(flac.bytes.compare(0, 4, "fLaC") == 0, "flac starts fLaC");
    expect(flac.bytes.size() < wav.bytes.size(), "flac is smaller than wav");
    expect(sim::base64_encode("Man") == "TWFu", "base64 encodes a full triple");
    expect(sim::base64_encode("Ma") == "TWE=", "base64 pads a two-byte tail");
    expect(sim::base64_encode("M") == "TQ==", "base64 pads a one-byte tail");
    expect(sim::base64_encode("") == "", "base64 of nothing is nothing");

    std::cout << (ok ? "audio encode: ok\n" : "audio encode: FAILED\n");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--check-audio-encode") == 0) {
        return check_audio_encode();
    }

    try {
        sim::Config config = sim::load_config();
        //read all settings from environment variables once at startup

        auto stt = std::make_unique<sim::WhisperSTT>(config.whisper_model_path);
        auto tts = std::make_unique<sim::PiperTTS>(
            config.piper_model_path, sim::configured_voice_paths(config));
        //build the concrete STT and TTS implementations
        //use unique_ptr
        //the voice list is passed so every voice's sample rate is read at
        //startup rather than on the turn that first uses it. It comes from the
        //same place Server's registry will read it, so the two cannot disagree

        auto store = std::make_unique<sim::Store>(config.database_path);
        //opened and migrated before run(), never inside it: prewarm_examiner
        //occupies every pool thread at startup, and a database opened after it
        //would be opened by whichever turn happened to need it first

        std::unique_ptr<sim::InterfaceExaminer> examiner;
        if (config.examiner_backend == sim::ExaminerBackend::Hailo) {
            examiner = std::make_unique<sim::HailoExaminer>(config.hailo_ollama_url);
        } else {
            examiner = std::make_unique<sim::GeminiExaminer>(
                config.gemini_api_keys,
                sim::GeminiSettings{config.gemini_model,
                                    config.gemini_thinking_level,
                                    config.gemini_opening_thinking_level});
        }
        //choose the examiner backend based on config - the ONLY place this is decided

        sim::Server server(std::move(config),
                           std::move(stt),
                           std::move(examiner),
                           std::move(tts),
                           std::move(store));
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
    //without this a throw from load_config, a missing model or a taken port
    //leaves main via std::terminate with no clear error messaging
}
