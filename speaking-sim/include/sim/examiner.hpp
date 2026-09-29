#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sim {

enum class Role {
    System,
    Examiner,
    Student,
};

struct Turn {
    Role role;
    std::string text;
};

// The student's recorded answer, encoded and ready to send. Empty bytes mean
// the caller has already transcribed it and the history carries the text, which
// is what every backend except Gemini expects.
struct SpokenAnswer {
    std::string bytes;
    std::string mime_type;
};

// What a backend heard and what it said back.
//
// transcript is filled only when the backend did the transcribing: a Gemini
// call handed audio returns the student's words alongside the reply, because
// it had to understand them to answer. It is empty when the caller transcribed
// first, and the caller keeps its own text in that case.
struct ExaminerReply {
    std::string transcript;
    std::string text;
    //the reply, still carrying its [topic: x] tag for topic_tag() to read

    std::vector<std::string> question_tenses;
    std::vector<std::string> answer_tenses;
    //canonical keys from tenses.hpp: the tenses the new question is phrased in,
    //and the ones the student used in the answer being replied to
    std::string required_question;
    //the id of the teacher's set question this reply asks, or empty
    std::optional<bool> asks_opinion;
    //the examiner's own word on whether it asked for an opinion. Empty from a
    //backend that cannot say, and Session then falls back to matching phrases
};

// What this session needs the reply to carry. Built by Session from its exam
// plan, so the fields and the values allowed in them follow the plan: the
// topic enum narrows to the plan's topics, the tense fields list the tenses in
// the exam language's own names, and the required-question field only exists
// while there is a set question still to ask.
struct ReplySchema {
    std::vector<std::string> topic_tags;
    //empty means every tag in kTopicTags
    std::vector<std::pair<std::string, std::string>> tenses;
    //canonical key -> this language's name for it. Empty leaves the tense
    //fields out of the reply altogether
    std::vector<std::string> required_ids;
    //the set questions still pending, as "q<id>"
};

class InterfaceExaminer {
public:
    virtual ~InterfaceExaminer() = default;

    //gemini_key_name selects among Config::gemini_api_keys; empty means "use
    //the default". Backends without a key concept (Hailo) ignore it
    virtual std::string respond(const std::vector<Turn>& history,
                                 const std::string& gemini_key_name = "") = 0;

    //As above, but the student's answer arrives as audio rather than as a
    //Student turn in the history. A backend that cannot listen inherits the
    //default below, which ignores the audio and answers from the text - so
    //adding a listening backend did not force every other one to grow a
    //capability it does not have
    virtual ExaminerReply respond_to_audio(const std::vector<Turn>& history,
                                           const SpokenAnswer& answer,
                                           const std::string& gemini_key_name,
                                           const ReplySchema& schema) {
        (void)answer;
        (void)schema;
        ExaminerReply reply;
        reply.text = respond(history, gemini_key_name);
        return reply;
    }
    //schema is ignored by a backend with no structured output: the reply then
    //carries no tenses or set-question id, and Session falls back to the rule
    //checks and word overlap it runs anyway

    //Whether respond_to_audio() does anything with the audio. The server asks
    //before encoding, so a backend that cannot listen does not pay for a FLAC
    //encode nobody reads
    virtual bool accepts_audio() const { return false; }

    //Opens whatever connection the backend needs, so the first real turn does
    //not pay for it. Called once per worker thread at startup, off the critical
    //path. Not pure: a backend with nothing to open inherits the empty default
    //rather than being forced to write one
    virtual void prewarm() {}
};

}  // namespace sim
