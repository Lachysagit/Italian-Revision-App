#include "sim/session.hpp"

#include "sim/topics.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace sim {

namespace {

bool has_visible_text(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    });
    //.empty() tests length alone, so a whitespace-only transcript went to the
    //model as a blank turn. The unsigned char cast is required by isspace
}

// Two is a follow-up, three is a short thread, four is a rabbit hole.
int draw_topic_budget(std::mt19937& rng) {
    std::uniform_int_distribution<int> pick(2, 3);
    return pick(rng);
}

// Late enough that the student has warmed up, early enough that a short exam
// still reaches it.
int draw_opinion_target(std::mt19937& rng) {
    std::uniform_int_distribution<int> pick(4, 6);
    return pick(rng);
}

bool asks_opinion(const std::string& reply) {
    std::string lowered;
    lowered.reserve(reply.size());
    for (const char ch : reply) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }

    for (const char* opener : {"secondo te", "cosa ne pensi", "che ne pensi",
                               "sei d'accordo"}) {
        if (lowered.find(opener) != std::string::npos) {
            return true;
        }
    }
    return false;
    //ASCII substrings only, so accented text folds past untouched
}

// The prompt files carry this where their topic list used to sit.
constexpr std::string_view kTopicMarker = "{{TOPICS}}";

std::string fill_topics(std::string text, std::mt19937& rng) {
    std::size_t at = text.find(kTopicMarker);
    while (at != std::string::npos) {
        const std::string menu = topic_menu(rng);
        text.replace(at, kTopicMarker.size(), menu);
        at = text.find(kTopicMarker, at + menu.size());
    }
    return text;
    //a file with no marker comes back untouched, so an out-of-date prompt on
    //disk still runs an exam rather than stopping the server
}

}  // namespace

Session::Session()
    : rng_(std::random_device{}()) {
    topic_budget_ = draw_topic_budget(rng_);
    //up front, so an examiner that never tags a reply is still moved on
    opinion_target_ = draw_opinion_target(rng_);
}

void Session::set_prompts(std::string first, std::string ongoing) {
    first_prompt_ = fill_topics(std::move(first), rng_);
    ongoing_prompt_ = fill_topics(std::move(ongoing), rng_);
    //the examiner only sees build_examiner_input(), so a prompt has to lead
    //the snapshot. The snapshot is rebuilt per call, so prompts cannot stack
    //the topic list is drawn here rather than per turn: the order holds for the
    //whole exam, so the examiner never sees the topics reshuffle under it
}

void Session::set_question_bank(std::shared_ptr<const QuestionBank> bank) {
    question_bank_ = std::move(bank);
    //shared_ptr to const, so every session reads the one copy loaded at startup
}

void Session::set_student_name(std::string name) {
    student_name_ = std::move(name);
}

void Session::set_gemini_key_name(std::string name) {
    gemini_key_name_ = std::move(name);
}

const std::string& Session::gemini_key_name() const {
    return gemini_key_name_;
}

void Session::record_answer(std::string answer) {
    last_answer_ = std::move(answer);
    //single destination, so the parameter is moved straight in
}

void Session::record_question(std::string question) {
    last_question_ = std::move(question);
    ++questions_asked_;

    if (!opinion_done_ && asks_opinion(last_question_)) {
        opinion_done_ = true;
        //an opinion asked without being told to still discharges the debt
    }
}

void Session::note_question_topic(const std::string& topic) {
    const std::string group = topic_group(topic);
    //the tag names a corner of a syllabus topic; the count runs on the topic

    const bool returning =
        std::find(covered_topics_.begin(), covered_topics_.end(), group) !=
        covered_topics_.end();

    if (group.empty() || group == current_topic_ || returning) {
        ++topic_questions_;
        return;
        //an untagged reply, or one still inside the running topic, sits on the
        //thread already running. Two cases had been resetting the count and
        //buying the examiner endless follow-ups: a tag outside the set, and a
        //hop back to a topic already finished. Neither is progress
    }

    if (has_visible_text(current_topic_)) {
        covered_topics_.push_back(current_topic_);
    }

    current_topic_ = group;
    topic_questions_ = 1;
    topic_budget_ = draw_topic_budget(rng_);
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

std::string Session::change_topic_directive() const {
    std::string directive = "You have now asked " +
                            std::to_string(topic_questions_) + " questions ";
    directive += has_visible_text(current_topic_)
                     ? "about \"" + current_topic_ + "\". That topic is finished."
                     : "on the same subject. That subject is finished.";
    directive +=
        " Your next question must open a different topic and must not refer "
        "back to what was just discussed. Ask it straight out, with no "
        "preamble and no linking sentence, and tag it with the new topic.";

    if (!covered_topics_.empty()) {
        directive += " Topics already covered, which you must not return to: ";
        for (std::size_t i = 0; i < covered_topics_.size(); ++i) {
            if (i != 0) {
                directive += ", ";
            }
            directive += covered_topics_[i];
        }
        directive += ".";
        //naming them is what stops the exam circling back once it runs dry
    }

    return directive;
}

std::string Session::opinion_directive() const {
    std::string directive =
        "This question must ask the student for an opinion rather than for a "
        "fact. Open it with \"Secondo te\" or an equivalent, and ask what the "
        "student thinks";

    directive += has_visible_text(current_topic_)
                     ? " about \"" + current_topic_ + "\"."
                     : " about what you are discussing.";
    directive +=
        " Stay on the topic already running, keep it to one short clause, and "
        "keep to one question mark. Tag it with the topic as usual.";

    return directive;
}

std::vector<Turn> Session::build_examiner_input() const {
    const bool opening_turn = !has_visible_text(last_question_);
    //nothing has been asked yet, so this is the first question of the exam

    std::vector<Turn> input;
    input.reserve(6);
    //at most prompt + name + samples + opinion + question + answer, so one
    //allocation

    input.push_back(
        Turn{Role::System, opening_turn ? first_prompt_ : ongoing_prompt_});
    //one prompt file or the other, never both. examiner_first.txt is the whole
    //instruction for the opening question and examiner_ongoing.txt is the whole
    //instruction for the rest, so neither can contradict the other
    //copies the prompt into the caller's vector, deliberately: the returned
    //Turns own their text and can outlive this Session's next write

    if (has_visible_text(student_name_)) {
        input.push_back(Turn{Role::System,
                             "Lo studente si chiama " + student_name_ +
                                 ". Sai gia come si chiama, quindi non "
                                 "chiedere mai il suo nome."});
        //a second System turn rather than an edit to system_prompt_: the file
        //on disk is shared by every session and must stay one student short of
        //complete. GeminiExaminer joins System turns with a blank line, and
        //this one is rebuilt per snapshot so it cannot stack across turns
    }
    //whitespace only counts as no name, the same test the transcript uses:
    //a stray space in the settings box must not become the student's name

    const bool changing_topic =
        !opening_turn && topic_questions_ >= topic_budget_;

    const bool opinion_due = !opening_turn && !opinion_done_ &&
                             !changing_topic &&
                             questions_asked_ >= opinion_target_;
    //held back on a turn that is already changing topic: one order per turn.
    //It is re-issued every later turn until a reply actually asks an opinion,
    //so an examiner that ignores it once does not lose the question

    if (changing_topic) {
        input.push_back(Turn{Role::System, change_topic_directive()});
        //the examiner only ever sees the last exchange, so the count lives here
    } else if (!opening_turn && question_bank_) {
        std::string examples = question_bank_->examples_for(
            current_topic_, kExampleQuestions, rng_);
        if (!examples.empty()) {
            input.push_back(Turn{Role::System, std::move(examples)});
        }
    }
    //never both. The samples are for the topic running, and the directive is
    //an order to leave it, so a turn carrying the two would contradict itself.
    //Nothing is sampled on the opening turn either: no reply has been tagged
    //yet, so there is no topic to draw from, and examiner_first.txt stands
    //alone as it did before
    //the first-turn instruction that used to sit here is gone: it repeated
    //examiner_first.txt in slightly different words, and the two drifted. The
    //only System turns the program still adds are the ones a file cannot
    //carry, because they depend on this session's own state

    if (opinion_due) {
        input.push_back(Turn{Role::System, opinion_directive()});
        //alongside the samples, not instead of them: those set the register,
        //this shapes the one question
    }

    if (has_visible_text(last_question_)) {
        input.push_back(Turn{Role::Examiner, last_question_});
    }
    if (has_visible_text(last_answer_)) {
        input.push_back(Turn{Role::Student, last_answer_});
    }
    //strings with no visible characters are skipped rather than sent as blank
    //turns. On the opening turn the examiner runs on the prompt alone

    return input;
    //by value. NRVO elides the copy, and even if it did not this would move
}

void Session::append_audio(const std::vector<std::int16_t>& chunk) {
    if (audio_buffer_.size() >= kMaxBufferedSamples) {
        return;
    }

    const std::size_t room = kMaxBufferedSamples - audio_buffer_.size();
    const std::size_t take = std::min(room, chunk.size());
    audio_buffer_.insert(audio_buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(take));
    //keeping earliest 40 seconds of buffer
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
