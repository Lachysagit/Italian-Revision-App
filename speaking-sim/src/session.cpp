#include "sim/session.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace sim {

Session::Session() = default;

void Session::set_system_prompt(std::string prompt) {
    system_prompt_ = std::move(prompt);
    //the examiner only ever sees build_examiner_input(), so the prompt has to
    //live in the snapshot as the leading turn or it never reaches the model at all
    //no replace-vs-insert branch is needed any more: the snapshot is rebuilt
    //from scratch every call, so calling this twice cannot stack prompts
}

void Session::record_answer(std::string answer) {
    last_answer_ = std::move(answer);
    //single destination, so the parameter is moved straight in
}

void Session::record_question(std::string question) {
    last_question_ = std::move(question);
}

bool Session::try_begin_job() {
    bool expected = false;
    return job_in_flight_.compare_exchange_strong(expected, true);
    //expected must be a fresh local, compare_exchange overwrites it on failure
}

void Session::end_job() {
    job_in_flight_.store(false);
    //seq_cst store, so this job's writes are visible to the next job's snapshot
}

std::vector<Turn> Session::build_examiner_input() const {
    std::vector<Turn> input;
    input.reserve(3);
    //at most system + question + answer, so one allocation and no regrowth

    input.push_back(Turn{Role::System, system_prompt_});
    //copies the prompt into the caller's vector. the copy is the point: the
    //returned Turns own their text, so the caller can outlive this Session's
    //next write without reading freed memory

    if (!last_question_.empty()) {
        input.push_back(Turn{Role::Examiner, last_question_});
    }
    if (!last_answer_.empty()) {
        input.push_back(Turn{Role::Student, last_answer_});
    }
    //empty strings are skipped rather than sent as blank turns. on the opening
    //turn both are empty and the examiner runs on the prompt alone

    return input;
    //by value. NRVO elides the copy, and even if it did not this would move
}

bool Session::append_audio(const std::vector<std::int16_t>& chunk) {
    if (audio_buffer_.size() >= kMaxBufferedSamples) {
        return false;
    }

    const std::size_t room = kMaxBufferedSamples - audio_buffer_.size();
    const std::size_t take = std::min(room, chunk.size());
    audio_buffer_.insert(audio_buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(take));
    //keeping earliest 40 seconds of buffer

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


}  // namespace sim
