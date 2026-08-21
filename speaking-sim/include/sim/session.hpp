#pragma once

#include <atomic>
#include <cstddef>
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

    std::vector<Turn> build_examiner_input() const;
    //returns an OWNED snapshot by value. the examiner must never hold a
    //reference into a Session across respond(), which can block for seconds

    bool try_begin_job();
    //claims this session for one pipeline job, false if a job already holds it
    void end_job();

    static constexpr std::size_t kCaptureSampleRate = 16000;
    static constexpr std::size_t kMaxBufferedSamples = 40 * kCaptureSampleRate;
    //40 seconds of capture. a client that streams audio and never sends Stop
    bool append_audio(const std::vector<std::int16_t>& chunk);
    //returns false if the cap was hit and some or all of the chunk was dropped
    bool audio_full() const;
    std::vector<std::int16_t> take_audio();

    void stash_partial_byte(std::string byte);
    std::string take_partial_byte();

private:
    std::atomic<bool> job_in_flight_{false};
    //one job at a time per session, so a turn is atomic and the strings below
    //are never touched by two threads at once

    std::string system_prompt_;
    std::string last_question_;
    std::string last_answer_;
    //the entire live conversation state. one prior exchange, no transcript
    //unguarded: the claim serialises every access, the atomic publishes them

    std::vector<std::int16_t> audio_buffer_;
    std::string partial_byte_;
    //a websocket frame can split a 16-bit sample down the middle, so the odd
    //trailing byte is held here and prepended to the next frame

    std::vector<std::string> fact_store_;
    //STUB, deliberately unused for now. the fact-extraction pass will populate
    //this with durable personal facts ("studies medicine", "has a brother in
    //Milan") and build_examiner_input will emit them as extra System turns
    //after the prompt. nothing here needs restructuring to switch it on
};

}  // namespace sim
