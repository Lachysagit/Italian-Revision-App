#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

class InterfaceTTS {
public:
    virtual ~InterfaceTTS() = default;

    virtual std::vector<std::int16_t> synthesize(const std::string& text) = 0;

    virtual int sample_rate() const = 0;
    //the rate synthesize() produces. The browser has no way to infer it, and
    //playing it back at the AudioContext rate shifts the pitch
};

}  // namespace sim
