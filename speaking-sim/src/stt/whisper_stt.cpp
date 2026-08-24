#include "sim/stt/whisper_stt.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef SIM_HAVE_WHISPER
#include "whisper.h"
#endif

namespace sim {

WhisperSTT::WhisperSTT(std::string model_path): model_path_(std::move(model_path)) {
#ifdef SIM_HAVE_WHISPER
    if (model_path_.empty()) {
        std::cerr << "WHISPER_MODEL_PATH is empty, speech to text is disabled\n";
        return;
        // An unset path means "no STT configured", the config default: ctx_
        // stays null and the server runs without a 74 MB download.
    }

    whisper_context_params cparams = whisper_context_default_params();
    ctx_ = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
    if (ctx_ == nullptr) {
        throw std::runtime_error("whisper model load failed: " + model_path_);

    }
#endif

}

WhisperSTT::~WhisperSTT() {
#ifdef SIM_HAVE_WHISPER
    if (ctx_ != nullptr) {
        whisper_free(ctx_);
    }
    // whisper_free matches whisper_init_from_file_*. Only reached with a
    // non-null ctx_, and copy/move are deleted, so it runs exactly once.
#endif
}

std::string WhisperSTT::transcribe(const std::vector<std::int16_t>& pcm) {
    if (pcm.empty()) {
        return {};
    
    }

#ifdef SIM_HAVE_WHISPER
    if (ctx_ == nullptr) {
        return {};
        // No model was configured. Same contract as the empty-audio case above.
    }


    std::vector<float> pcmf32(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcmf32[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }

    whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = "it";
    wparams.translate = false;
    wparams.no_timestamps = true;
    wparams.print_progress = false; 
    wparams.print_realtime = false;
    wparams.print_special = false;
    // All cores but one. hardware_concurrency() may return 0, and 0u - 1 wraps,
    // so the subtraction only happens when there is something to subtract from.
    const unsigned int cores = std::thread::hardware_concurrency();
    wparams.n_threads = static_cast<int>(cores > 1 ? cores - 1 : 1);

    std::string transcript; //variable where Whisper transcript is stored
    {
        std::lock_guard<std::mutex> lock(mutex_);
    

        if (whisper_full(ctx_, wparams, pcmf32.data(),
                         static_cast<int>(pcmf32.size())) != 0) {
            throw std::runtime_error("whisper_full (transcription) failed");
        }

        const int segments = whisper_full_n_segments(ctx_); //returns count of segments whisper found
        for (int i = 0; i < segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx_, i);
            if (text != nullptr) transcript += text;
            // Appending copies the characters out of the context's own storage,
            // so nothing borrowed from ctx_ escapes this scope.
        }
    }
    // The lock is released here: the trimming below works on this call's own
    // string, so the next turn can start decoding while this one tidies up.

    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t begin = 0;
    while (begin < transcript.size() &&
           is_space(static_cast<unsigned char>(transcript[begin]))) {
        ++begin;
    }
    std::size_t end = transcript.size();
    while (end > begin &&
           is_space(static_cast<unsigned char>(transcript[end - 1]))) {
        --end;
    }
    return transcript.substr(begin, end - begin);
#else
    std::cerr << "WhisperSTT::transcribe not implemented\n";
    return "placeholder transcript";
#endif
}

}  // namespace sim
