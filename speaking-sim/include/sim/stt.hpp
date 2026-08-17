#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

class InterfaceSTT {
public:

    virtual ~ISTT() = default;

    virtual std::string transcribe(const std::vector<std::int16_t>& pcm) = 0;

};

}  // namespace sim