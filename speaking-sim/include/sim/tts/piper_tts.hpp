#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/tts.hpp"

namespace sim {

class PiperTTS : public InterfaceTTS { //inheritance
public:
    explicit PiperTTS(std::string model_path);

    std::vector<std::int16_t> synthesize(const std::string& text) override;
    //override pure virtual synthesize method of base class

    int sample_rate() const override;
    //override pure virtual sample_rate method of base class

private:
    std::string model_path_;
};

}  // namespace sim
