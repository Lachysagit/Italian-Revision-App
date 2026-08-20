#include "sim/session.hpp"

#include <utility>

namespace sim {

Session::Session() = default;

void Session::set_system_prompt(std::string prompt) {
    system_prompt_ = std::move(prompt);

    const Turn system_turn{Role::System, system_prompt_};
    //the examiner only ever sees get_history(), so the prompt has to live in
    //the history as the leading turn or it never reaches the model at all
    if (!history_.empty() && history_.front().role == Role::System) {
        history_.front() = system_turn;
        //replace rather than insert, so calling this twice cannot stack prompts
    } else {
        history_.insert(history_.begin(), system_turn);
    }
}

void Session::record_answer(std::string answer) {
    last_answer_ = answer;
    history_.push_back(Turn{Role::Student, std::move(answer)});
}

void Session::record_question(std::string question) {
    last_question_ = question;
    history_.push_back(Turn{Role::Examiner, std::move(question)});
}

const std::vector<Turn>& Session::get_history() const {
    return history_;
}

void Session::append_audio(const std::vector<std::int16_t>& chunk) {
    audio_buffer_.insert(audio_buffer_.end(), chunk.begin(), chunk.end());
}

std::vector<std::int16_t> Session::take_audio() {
    std::vector<std::int16_t> result = std::move(audio_buffer_);
    audio_buffer_.clear();
    partial_byte_.clear();
    //the utterance is over, so a half sample left from its last frame belongs
    //to nothing and must not be glued onto the start of the next utterance
    return result;
}

void Session::stash_partial_byte(std::string byte) {
    partial_byte_ = std::move(byte);
}

std::string Session::take_partial_byte() {
    std::string result = std::move(partial_byte_);
    partial_byte_.clear();
    return result;
}


}  // namespace sim