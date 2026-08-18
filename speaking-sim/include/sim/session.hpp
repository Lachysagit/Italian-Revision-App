#pragma once

#include <string>
#include <vector>

#include "sim/examiner.hpp"

namespace sim {

class Session {
public:
    Session();
    void set_system_prompt(std::string prompt);

    void record_answer(std::string answer);
    void record_question(std::string question);

    void append_audio(const std::vector<std::int16_t>& chunk);
    std::vector<std::int16_t> take_audio();

private:
    std::string system_prompt_;
    std::string last_question_;
    std::string last_answer_;

    std::vector<std::int16_t> audio_buffer_;

    std::vector<std::string> fact_store_;
};

}  // namespace sim