#include "sim/session.hpp"

#include <algorithm>
#include <cstddef>
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

bool Session::append_audio(const std::vector<std::int16_t>& chunk) {
    if (audio_buffer_.size() >= kMaxBufferedSamples) {
        return false;
    }

    const std::size_t room = kMaxBufferedSamples - audio_buffer_.size();
    const std::size_t take = std::min(room, chunk.size());
    audio_buffer_.insert(audio_buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(take));
    //the head of the utterance is kept and the overflowing tail dropped. keeping
    //the newest 40 s instead would mean erasing from the front of the vector on
    //every frame, and would leave STT with speech that starts mid-word

    return take == chunk.size();
}

bool Session::audio_full() const {
    return audio_buffer_.size() >= kMaxBufferedSamples;
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

bool Session::try_begin_job() {
    bool expected = false;
    return job_in_flight_.compare_exchange_strong(expected, true);
    //flip false to true and report whether this caller was the one that did it.
    //expected has to be a fresh local every call: compare_exchange_strong writes
    //the observed value back into it when the exchange fails, so a reused or
    //shared variable would come back holding true and never match again
}

void Session::end_job() {
    job_in_flight_.store(false);
    //the session is free again, the next Start or Stop can claim it
}


}  // namespace sim