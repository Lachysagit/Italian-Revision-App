#pragma once

#include <string>
#include <vector>

#include "sim/examiner.hpp"

namespace sim {

class GeminiExaminer : public InterfaceExaminer {
public:
    explicit GeminiExaminer(std::string api_key);

    std::string respond(const std::vector<Turn>& history) override;

private:
    std::string api_key_;
};

}  // namespace sim