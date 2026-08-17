#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

class InterfaceTTS {
public:
    virtual ~InterfaceTTS() = default;

    virtual std::vector<std::int16_t> synthesize(const std::string& text) = 0;
};

}  // namespace sim