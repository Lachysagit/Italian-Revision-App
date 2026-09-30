#include <cmath>
#include <cstdlib>
#include <ctime>
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
#include "sim/tenses.hpp"

#include "sim/examiner/gemini_examiner.hpp"
#include "sim/examiner/hailo_examiner.hpp"
#include "sim/safety/azure_safety.hpp"
#include "sim/safety/examiner_adjudicator.hpp"
#include "sim/safety/semantic_adjudicator.hpp"
#include "sim/safety/safety_chain.hpp"
#include "sim/safety/wordlist_safety.hpp"
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

bool is_date(const std::string& text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}

std::string format_day(std::int64_t seconds) {
    const std::time_t t = static_cast<std::time_t>(seconds);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", std::localtime(&t));
    return buffer;
}

// Licence administration: paid access is granted from here until a payment
// provider exists to write the same rows. Runs against DATABASE_PATH and exits
// without starting the server, so it is safe beside a running one - SQLite's
// WAL lets both at the file.
int licence_command(int argc, char** argv) {
    const std::string command = argv[1];
    sim::Config config = sim::load_config();
    sim::Store store(config.database_path);

    if (command == "--list-licences") {
        for (const sim::Licence& licence : store.licences()) {
            std::cout << licence.id << "  " << licence.kind << " " << licence.target_id
                      << " (" << licence.target_label << ")  "
                      << format_day(licence.starts_at) << " to "
                      << format_day(licence.ends_at)
                      << (licence.revoked ? "  REVOKED" : "")
                      << (licence.note.empty() ? "" : "  - " + licence.note) << "\n";
        }
        return 0;
    }

    if (command == "--revoke-licence") {
        if (argc < 3) {
            std::cerr << "usage: speaking-sim --revoke-licence <id>\n";
            return 2;
        }
        const bool revoked = store.revoke_licence(std::atoll(argv[2]));
        std::cout << (revoked ? "revoked\n" : "no such active licence\n");
        return revoked ? 0 : 1;
    }

    // --grant-licence user <email> <YYYY-MM-DD> [note]
    // --grant-licence class <class id> <YYYY-MM-DD> [note]
    if (argc < 5) {
        std::cerr << "usage: speaking-sim --grant-licence user <email> <YYYY-MM-DD> [note]\n"
                     "       speaking-sim --grant-licence class <class id> <YYYY-MM-DD> [note]\n";
        return 2;
    }
    const std::string kind = argv[2];
    const std::string target = argv[3];
    const std::string until = argv[4];
    const std::string note = argc > 5 ? argv[5] : "";
    if (!is_date(until)) {
        std::cerr << "the end date must be YYYY-MM-DD\n";
        return 2;
    }

    std::int64_t target_id = 0;
    if (kind == "user") {
        const auto user = store.user_by_email(target);
        if (!user) {
            std::cerr << "no account with the email " << target
                      << " - they have to sign in once first\n";
            return 1;
        }
        target_id = user->id;
    } else if (kind == "class") {
        target_id = std::atoll(target.c_str());
        if (!store.class_by_id(target_id)) {
            std::cerr << "no class with id " << target << "\n";
            return 1;
        }
    } else {
        std::cerr << "kind must be user or class\n";
        return 2;
    }

    const std::int64_t id = store.grant_licence(kind, target_id, until, note);
    std::cout << "licence " << id << ": " << kind << " " << target << " until "
              << until << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--check-audio-encode") == 0) {
        return check_audio_encode();
    }
    if (argc > 1 && (std::strcmp(argv[1], "--grant-licence") == 0 ||
                     std::strcmp(argv[1], "--revoke-licence") == 0 ||
                     std::strcmp(argv[1], "--list-licences") == 0)) {
        try {
            return licence_command(argc, argv);
        } catch (const std::exception& error) {
            std::cerr << "fatal: " << error.what() << std::endl;
            return 1;
        }
    }
    if (argc > 1 && std::strcmp(argv[1], "--check-tenses") == 0) {
        return sim::check_tense_rules();
        //the tense rules feed the teacher's report, and a rule that misfires
        //does not crash - it quietly credits a student with the wrong tense.
        //This runs them over known sentences without a server or a student
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

        std::vector<std::unique_ptr<sim::InterfaceSafety>> layers;
        if (config.safety_mode != sim::SafetyMode::Off) {
            layers.push_back(std::make_unique<sim::WordlistSafety>(
                config.safety_wordlist_dir));
            //the local layer is always first: a word on the list costs no
            //network hop, and on the offline build there is no second layer
        }
        if (config.safety_mode == sim::SafetyMode::Azure) {
            sim::AzureSafety::Options options;
            options.endpoint = config.content_safety_endpoint;
            options.api_key = config.content_safety_key;
            options.halt_severity = config.content_safety_halt_severity;
            options.shield_prompts = config.safety_shield_prompts;
            layers.push_back(std::make_unique<sim::AzureSafety>(
                std::move(options)));
        }
        std::unique_ptr<sim::SemanticAdjudicator> adjudicator;
        if (config.adjudicator_mode == sim::AdjudicatorMode::Examiner) {
            sim::SemanticAdjudicator::Options options;
            options.categories = config.adjudicate_categories;
            options.self_harm = config.adjudicate_self_harm;
            adjudicator = std::make_unique<sim::SemanticAdjudicator>(
                std::make_unique<sim::ExaminerAdjudicator>(examiner.get(), ""),
                std::move(options));
            //the adjudicator borrows the examiner, which Server is about to
            //take ownership of. That is safe because Server owns both for the
            //same lifetime and destroys the chain before the examiner - but it
            //is the one raw pointer in this wiring, so it is spelled out here
            //rather than left to be discovered
        }

        auto safety = std::make_unique<sim::SafetyChain>(
            std::move(layers),
            sim::SafetyChain::Options{config.safety_fail_closed},
            std::move(adjudicator));
        //built here beside the examiner for the same reason: this is the ONLY
        //place the safety backends are chosen. An empty chain is what
        //SAFETY_MODE=off produces, and its ready() is false, which is what
        //makes an unscreened build refuse to start once AUTH_REQUIRED is on

        sim::Server server(std::move(config),
                           std::move(stt),
                           std::move(examiner),
                           std::move(tts),
                           std::move(store),
                           std::move(safety));
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
