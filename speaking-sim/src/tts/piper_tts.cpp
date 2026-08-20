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

int PiperTTS::sample_rate() const {
    return 22050;
    //piper's own default. Once the model is loaded this should come from the
    //voice's config rather than being hard coded here
}

}  // namespace sim
