#pragma once

#include <string>
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
                                           const std::string& gemini_key_name = "") {
        (void)answer;
        return {"", respond(history, gemini_key_name)};
    }

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
