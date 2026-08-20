#include "sim/stt/whisper_stt.hpp"

#include <iostream>
#include <utility>

namespace sim {

WhisperSTT::WhisperSTT(std::string model_path): model_path_(std::move(model_path)) {
}

std::string WhisperSTT::transcribe(const std::vector<std::int16_t>& pcm) {
    std::cerr << "WhisperSTT::transcribe not implemented\n";
    return "placeholder transcript";
}

}  // namespace sim