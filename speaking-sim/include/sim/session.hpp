#pragma once

#include <string>
#include <vector>

#include "sim/examiner.hpp"

namespace sim {

class Session {
public:
    Session();

private:
    std::string system_prompt_;
    std::string last_question_;
    std::string last_answer_;

    std::vector<std::int16_t> audio_buffer_;

    std::vector<std::string> fact_store_;
};

}  // namespace sim