#pragma once

#include <atomic>
#include <memory>
#include <random>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/examiner.hpp"
#include "sim/question_bank.hpp"

namespace sim {

class Session {
public:
    Session();
    void set_prompts(std::string first, std::string ongoing);
    //the opening question and every later turn get one file each, and the
    //snapshot carries whichever applies. Nothing merges them: two prompts in
    //one request is how the length rule ended up contradicting itself before

    void set_question_bank(std::shared_ptr<const QuestionBank> bank);
    //the syllabus questions, shared read-only by every session. Only questions
    //for the topic already running are ever shown, and only on a turn that is
    //staying on it

    void set_student_name(std::string name);
    //taken from the Start message alongside the key, and folded into every
    //snapshot from then on. Empty is a valid answer: the examiner is simply
    //told nothing rather than being handed a blank name to greet
    void set_gemini_key_name(std::string name);
    const std::string& gemini_key_name() const;
    //picked once from the Start message and reused by every turn in this
    //session, so switching examiners mid-exam would need a new session

    void record_answer(std::string answer);
    void record_question(std::string question);

    void note_question_topic(const std::string& topic);
    //from the [topic: ...] tag the student never sees. Two or three questions
    //per topic, then build_examiner_input tells the examiner it is finished.
    //An empty topic counts against the topic already running

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
    std::string change_topic_directive() const;
    std::string opinion_directive() const;

    std::atomic<bool> job_in_flight_{false};


    std::shared_ptr<const QuestionBank> question_bank_;
    static constexpr std::size_t kExampleQuestions = 6;
    //enough to set a register, few enough that the examiner still reacts to
    //the student rather than working down a list

    std::string first_prompt_;
    std::string ongoing_prompt_;
    std::string last_question_;
    std::string last_answer_;
    std::string gemini_key_name_;
    std::string student_name_;

    std::string current_topic_;
    std::vector<std::string> covered_topics_;
    int topic_questions_ = 0;
    int topic_budget_;

    int questions_asked_ = 0;
    int opinion_target_;
    bool opinion_done_ = false;
    //every exam owes the student one question asking for an opinion. The turn
    //it falls on is drawn per session, so it is not the same beat every time
    //drawn per topic, so the examiner does not move on to a predictable rhythm
    mutable std::mt19937 rng_;
    //mutable because build_examiner_input() is const and draws its sample from
    //it. One job at a time holds a session, so the draws cannot race
 

    std::vector<std::int16_t> audio_buffer_;
    std::string partial_byte_;


    std::vector<std::string> fact_store_;
    //STUB for now
};

}  // namespace sim
