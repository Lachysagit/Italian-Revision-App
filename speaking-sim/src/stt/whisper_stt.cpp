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
        // An unset path means "no STT configured", which is the config default.
        // ctx_ stays null and transcribe() returns nothing, so the server still
        // starts and the examiner still asks questions - it just never hears an
        // answer. That keeps the web client runnable without a 74 MB download.
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
    // whisper_free is the matching release for whisper_init_from_file_*. It is
    // only reached with a non-null ctx_, and copy/move are deleted, so it runs
    // exactly once per successfully loaded model.
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
    // Thread count could move to config later.
    wparams.n_threads = static_cast<int>(
        std::max(1u, std::min(4u, std::thread::hardware_concurrency())));

    std::string transcript;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // whisper_full writes its decoded segments into state owned by ctx_, and
        // whisper_full_n_segments / whisper_full_get_segment_text read that same
        // state back. Both the call and the read therefore have to be inside one
        // critical section: releasing between them would let a second worker's
        // whisper_full overwrite the segments before this one had copied them
        // out, and the two students would swap answers.

        if (whisper_full(ctx_, wparams, pcmf32.data(),
                         static_cast<int>(pcmf32.size())) != 0) {
            throw std::runtime_error("whisper_full (transcription) failed");
        }

        const int segments = whisper_full_n_segments(ctx_);
        for (int i = 0; i < segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx_, i);
            if (text != nullptr) transcript += text;
            // Appending copies the characters out of the context's own storage,
            // so nothing borrowed from ctx_ escapes this scope.
        }
    }
    // The lock is released here. The trimming below works on transcript alone,
    // which is this call's own string, so the next turn can start decoding while
    // this one finishes tidying its result.

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
