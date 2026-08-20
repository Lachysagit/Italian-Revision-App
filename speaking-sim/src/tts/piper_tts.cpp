#include "sim/tts/piper_tts.hpp"

#include <iostream>
#include <utility>

namespace sim {

PiperTTS::PiperTTS(std::string model_path): model_path_(std::move(model_path)) {
}

std::vector<std::int16_t> PiperTTS::synthesize(const std::string& text) {
    std::cerr << "PiperTTS::synthesize not implemented\n";
    return std::vector<std::int16_t>{};
    //empty audio - nothing to play back
}

}  // namespace sim
