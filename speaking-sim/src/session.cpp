#include "sim/session.hpp"

#include <utility>

namespace sim {

Session::Session() = default;

void Session::set_system_prompt(std::string prompt) {
    system_prompt_ = std::move(prompt);
}

void Session::record_answer(std::string answer) {
    last_answer_ = std::move(answer);
}

void Session::record_question(std::string question) {
    last_question_ = std::move(question);
}

void Session::append_audio(const std::vector<std::int16_t>& chunk) {
    audio_buffer_.insert(audio_buffer_.end(), chunk.begin(), chunk.end());
}

std::vector<std::int16_t> Session::take_audio() {
    std::vector<std::int16_t> result = std::move(audio_buffer_);
    audio_buffer_.clear();
    return result;
}


}  // namespace sim