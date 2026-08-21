#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/tts.hpp"

namespace sim {

class PiperTTS : public InterfaceTTS {
public:
    explicit PiperTTS(std::string model_path);
    //piper's -m/--model is mandatory, so the voice has to be known here. The
    //old default constructor could not be given one, and main.cpp was already
    //passing config.piper_model_path to it

    std::vector<std::int16_t> synthesize(const std::string& text) override;

    int sample_rate() const override;
    //declared because InterfaceTTS::sample_rate is pure virtual: without it
    //PiperTTS stays abstract and make_unique<PiperTTS> does not compile

private:
    std::string model_path_;
    int sample_rate_ = 22050;
    //read once from <model_path>.json at construction. piper takes the rate
    //from the voice's own config, so a hardcoded value pitch-shifts any voice
    //that disagrees. 22050 is piper's own fallback when the key is absent
};

}  // namespace sim
