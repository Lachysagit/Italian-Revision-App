#include "sim/stt/whisper_stt.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <memory>
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
}

std::string WhisperSTT::transcribe(const std::vector<std::int16_t>& pcm) {
    if (pcm.empty()) {
        return {};
        // A Stop with nothing buffered (the student pressed Finished Response
        // without speaking) reaches here with no samples. whisper_full on a
        // zero-length buffer has nothing to decode, and loading a 74 MB model
        // to discover that costs seconds. The caller already treats an empty
        // transcript as "the student said nothing" and omits the Student turn.
    }

#ifdef SIM_HAVE_WHISPER
    // whisper needs 16 kHz mono float32 in [-1, 1] and does not resample
    // internally. client.js downsamples to 16 kHz and captures a single
    // channel, so the only work here is the integer to float conversion.
    std::vector<float> pcmf32(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcmf32[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }

    // The context is loaded and freed per call to avoid a header change and to
    // sidestep sharing one whisper_context across worker threads. Loading once
    // in the constructor with a mutex-guarded whisper_full and a destructor
    // free (copy/move deleted) is a later optimization.
    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* raw =
        whisper_init_from_file_with_params(model_path_.c_str(), cparams);
    if (raw == nullptr) {
        throw std::runtime_error("whisper model load failed: " + model_path_);
    }
    std::unique_ptr<whisper_context, void(*)(whisper_context*)> ctx(
        raw, whisper_free);

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

    if (whisper_full(ctx.get(), wparams, pcmf32.data(),
                     static_cast<int>(pcmf32.size())) != 0) {
        throw std::runtime_error("whisper_full (transcription) failed");
    }

    std::string transcript;
    const int segments = whisper_full_n_segments(ctx.get());
    for (int i = 0; i < segments; ++i) {
        const char* text = whisper_full_get_segment_text(ctx.get(), i);
        if (text != nullptr) transcript += text;
    }

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
