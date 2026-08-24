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


    bool try_begin_job();
    //claims this session for one pipeline job, false if a job already holds it
    void end_job();

    static constexpr std::size_t kCaptureSampleRate = 16000;
    static constexpr std::size_t kMaxBufferedSamples = 40 * kCaptureSampleRate;
    //40 seconds of capture. a client that streams audio and never sends Stop
    void append_audio(const std::vector<std::int16_t>& chunk);
    //silently drops whatever does not fit under the cap. callers detect the
    //cap through the audio_full() transition rather than a per-call result
    bool audio_full() const;
    std::vector<std::int16_t> take_audio();

    void stash_partial_byte(std::string byte);
    std::string take_partial_byte();

private:
    std::atomic<bool> job_in_flight_{false};


    std::string system_prompt_;
    std::string last_question_;
    std::string last_answer_;
 

    std::vector<std::int16_t> audio_buffer_;
    std::string partial_byte_;


    std::vector<std::string> fact_store_;
    //STUB for now
};

}  // namespace sim
