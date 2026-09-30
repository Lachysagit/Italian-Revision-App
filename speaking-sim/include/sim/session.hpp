#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/exam_plan.hpp"
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

    void set_user_id(std::int64_t id);
    std::optional<std::int64_t> user_id() const;
    //who is sitting the exam, read from the session cookie when the websocket
    //was accepted. nullopt for a browser that never signed in, which only a
    //server with AUTH_REQUIRED off lets through
    void set_question_limit(int limit);
    int question_limit() const;
    //today's allowance of examiner questions for the account sitting this
    //exam, fixed at Start. -1 means unmetered: a browser that never signed in,
    //which only a server with AUTH_REQUIRED off lets through
    void set_class_id(std::int64_t id);
    std::optional<std::int64_t> class_id() const;
    //the class the Start message named, once Server has checked the user is in
    //it. nullopt is private practice

    void set_attempt_id(std::int64_t id);
    std::int64_t attempt_id() const;
    //the exam_attempts row this session's turns are written against, opened by
    //Server when the exam starts. Zero means "not being recorded": the insert
    //failed, and a turn that cannot be stored still has to be answered

    using Clock = std::chrono::steady_clock;

    bool clock_started() const;
    void start_clock(Clock::time_point now, Clock::duration length);
    bool time_up(Clock::time_point now) const;
    //the exam's own deadline. Started once, when the opening question has gone
    //out, and read when each answer arrives: an answer submitted after it is
    //transcribed but never earns another question, whatever the browser says
    void pause_clock(Clock::time_point now);
    void resume_clock(Clock::time_point now);
    //the exam page's Pause button. The time left is banked on pause and a new
    //deadline is bought from it on resume, the same arithmetic the browser's
    //countdown does, so the two clocks stay in step across a pause. A paused
    //clock never runs out

    // ---- the exam plan ---------------------------------------------------

    void set_plan(ExamPlan plan);
    //the teacher's plan for this exam, set from Start before the first job.
    //Narrows the topics, queues the set questions and the tense targets, and
    //can switch the opinion question off. Without one the exam runs as it
    //always has
    const std::optional<ExamPlan>& plan() const;
    int plan_duration_seconds() const;
    //0 when there is no plan or it keeps the server's default length

    ReplySchema reply_schema() const;
    //what this turn's reply must carry: the plan's topic tags, this language's
    //tense names, and the set questions still to ask. Read right after
    //build_examiner_input, which may have just ordered one of them

    struct ReplyOutcome {
        std::vector<std::int64_t> asked;
        std::vector<std::int64_t> missed;
        //set questions this reply asked, and ones given up on after the
        //examiner ignored the order too many times
        std::vector<std::string> question_tenses;
        //model and rules together, as counted towards the plan's targets
        std::optional<std::string> opinion_source;
        //set on the one turn that discharges the exam's opinion question, and
        //empty on every other: "model" when the examiner said so itself,
        //"openers" when only the phrase list saw it. Which of the two found it
        //is worth keeping - it is the only measure of how much the phrase list
        //is actually catching, and the list is the weaker of the two checks
    };
    ReplyOutcome note_examiner_reply(const std::string& question,
                                     const ExaminerReply& reply);
    //after record_question: checks the reply against the set questions, counts
    //its tenses and decides whether this reply asked for an opinion. Returns
    //what Server has to write down

    int next_turn_index();
    //monotonic per session, handed to each stored turn. Not atomic on purpose -
    //the one-job latch already means a single thread touches a Session at a time

    int peek_turn_index() const { return turn_index_; }
    //the index the NEXT stored turn will take, without taking it. A safety
    //event belongs to the turn it screened rather than to one of its own, so
    //recording one must not consume an index - doing so would leave a gap in
    //attempt_turns and file the event against a turn that never existed

    void note_question_topic(const std::string& topic);
    //from the [topic: ...] tag the student never sees. Two or three questions
    //per topic, then build_examiner_input tells the examiner it is finished.
    //An empty topic counts against the topic already running

    std::vector<Turn> build_examiner_input();
    //not const: choosing this turn's one instruction is a decision the reply
    //is then checked against, so the choice is remembered


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
    std::string required_directive(const PlanQuestion& question, bool opening) const;
    std::string tense_directive(const std::string& tense) const;
    void rebuild_prompts();
    //fills both prompt files from the language pack and the plan together, so
    //set_language and set_plan can be called in either order

    struct RequiredState {
        PlanQuestion question;
        std::string key;
        //"q<id>", the value the reply names it by
        bool done = false;
        int orders = 0;
        //how many turns it has been ordered on. Given up at kMaxOrders, so an
        //examiner that will not ask it cannot hold the whole exam hostage
    };
    static constexpr int kMaxOrders = 3;

    bool pending(const RequiredState& state) const;
    std::optional<std::size_t> pick_required(bool changing_topic, bool urgent) const;
    std::optional<std::string> tense_due(int remaining) const;
    int remaining_turns(Clock::time_point now) const;
    //an estimate from the time left and the pace so far; -1 before the clock
    //has started

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
    bool opinion_required_ = true;
    //whether this exam owes one at all: a plan can switch it off. Kept apart
    //from opinion_done_, which it would be easier to pre-set, because that
    //would also switch the detection off - and then a plan wanting no opinion
    //question and an exam that asked one anyway would leave the same record
    mutable std::mt19937 rng_;
    //mutable because build_examiner_input() is const and draws its sample from
    //it. One job at a time holds a session, so the draws cannot race
 

    std::vector<std::int16_t> audio_buffer_;
    std::string partial_byte_;


    mutable std::mutex clock_mutex_;
    std::optional<Clock::time_point> deadline_;
    std::optional<Clock::duration> paused_left_;
    Clock::time_point clock_started_at_;
    std::optional<Clock::time_point> paused_at_;
    Clock::duration paused_total_{};
    //with clock_started_at_, what remaining_turns() measures the pace from:
    //time spent paused is not time spent answering

    std::optional<ExamPlan> plan_;
    std::vector<RequiredState> required_;
    std::optional<std::size_t> ordered_required_;
    //the set question this turn's input ordered, checked against the reply
    std::vector<std::pair<std::string, int>> tenses_asked_;
    int last_tense_order_ = -10;
    //the question count at which a tense was last ordered, so the orders are
    //spread across the exam rather than stacked on consecutive turns
    //set while paused. Guarded by its own mutex rather than the job latch:
    //Pause and Resume, like End, are handled without taking the latch, so they
    //can land while the opening job is starting the clock on a worker
    std::int64_t user_id_ = 0;
    std::int64_t class_id_ = 0;
    int question_limit_ = -1;
    //zero for "none": row ids start at 1, so zero is never a real one
    std::int64_t attempt_id_ = 0;
    int turn_index_ = 0;
    //plain data, deliberately: Session holds no pointer to the Store. The
    //registry it does point at is const after startup, the Store is not, and
    //keeping the I/O in Server is what stops a state object growing a database

    std::vector<std::string> fact_store_;
    //STUB for now
};

}  // namespace sim
