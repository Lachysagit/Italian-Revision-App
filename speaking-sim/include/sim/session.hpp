#pragma once

#include <cstdint>
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

    const std::vector<Turn>& get_history() const;

    void append_audio(const std::vector<std::int16_t>& chunk);
    std::vector<std::int16_t> take_audio();

    void stash_partial_byte(std::string byte);
    std::string take_partial_byte();

private:
    std::string system_prompt_;
    std::string last_question_;
    std::string last_answer_;

    std::vector<Turn> history_;

    std::vector<std::int16_t> audio_buffer_;
    std::string partial_byte_;
    //a websocket frame can split a 16-bit sample down the middle, so the odd
    //trailing byte is held here and prepended to the next frame

    std::vector<std::string> fact_store_;
};

}  // namespace sim