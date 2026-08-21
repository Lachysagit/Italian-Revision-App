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

    const std::vector<Turn>& get_history() const;

    static constexpr std::size_t kCaptureSampleRate = 16000;
    static constexpr std::size_t kMaxBufferedSamples = 40 * kCaptureSampleRate;
    //40 seconds of capture. a client that streams audio and never sends Stop
    //would otherwise grow audio_buffer_ for as long as the socket stays open
    bool append_audio(const std::vector<std::int16_t>& chunk);
    //returns false if the cap was hit and some or all of the chunk was dropped
    bool audio_full() const;
    std::vector<std::int16_t> take_audio();

    void stash_partial_byte(std::string byte);
    std::string take_partial_byte();

    bool try_begin_job();
    void end_job();
    //only one pipeline job may touch a session at a time. try_begin_job() claims
    //the session and reports whether this caller won it, end_job() hands it back

    class JobGuard {
    public:
        explicit JobGuard(Session& session) : session_(session) {}
        ~JobGuard() { session_.end_job(); }

        JobGuard(const JobGuard&) = delete;
        JobGuard& operator=(const JobGuard&) = delete;
        //the claim is owned by exactly one guard, so copying it would release
        //the session early and let a second job in while the first still runs

    private:
        Session& session_;
    };
    //the release has to happen even if the STT, examiner or TTS call throws.
    //a leaked claim is not recoverable: every later Start and Stop on this
    //session would be refused for the life of the connection

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

    std::atomic<bool> job_in_flight_{false};
    //the default seq_cst ordering here is doing real work, not just making the
    //flag itself race-free. the store in end_job() releases, the compare
    //exchange in try_begin_job() acquires, so everything the finishing job
    //wrote to history_ is visible to the next job even though the two run on
    //different pool threads. without that edge the jobs would be serialised but
    //the history handed between them would still be published unsynchronised
};

}  // namespace sim