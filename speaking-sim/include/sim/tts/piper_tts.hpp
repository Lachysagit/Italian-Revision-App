#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/tts.hpp"

namespace sim {

class PiperTTS : public InterfaceTTS {
public:
    PiperTTS();

    std::vector<std::int16_t> synthesize(const std::string& text) override;
};

}  // namespace sim