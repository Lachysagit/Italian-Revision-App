#pragma once

#include <atomic>
#include <memory>
#include <random>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/examiner.hpp"
#include "sim/language.hpp"
#include "sim/question_bank.hpp"

namespace sim {

class Session {
public:
    Session();
    void set_language(const LanguagePack* pack);
    //the exam's language, and with it the two prompt files, the syllabus
    //questions and every per-language string the program itself writes. Sets
    //the prompts too, filling {{TOPICS}} from this session's own draw: the
    //opening question and every later turn get one file each, and the snapshot
    //carries whichever applies. Nothing merges them, because two prompts in one
    //request is how the length rule ended up contradicting itself before.
    //Called once on open with the default and again from Start if the browser
    //named a language, so a client that never names one still runs an exam

    const LanguagePack& language() const;

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


    const LanguagePack* language_ = nullptr;
    //a bare pointer on purpose: the registry is a member of Server, is const
    //once loaded and outlives every session, so there is no ownership to share.
    //Null only between construction and the set_language() that Server's onopen
    //makes before the socket can carry a message

    std::shared_ptr<const QuestionBank> question_bank_;
    //copied out of the pack by set_language(), so a turn does not chase two
    //pointers to reach the questions
    static constexpr std::size_t kExampleQuestions = 6;
    //enough to set a register, few enough that the examiner still reacts to
    //the student rather than working down a list

    std::string first_prompt_;
    std::string ongoing_prompt_;
    //the pack's prompts with this session's topic order filled in, so they
    //cannot be read straight from the shared pack
    std::string opening_topic_;
    //the group that order put first. The opening turn draws its sample
    //questions from this one group and is told to open on it, which is what
    //makes the shuffle reach the question actually asked
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
