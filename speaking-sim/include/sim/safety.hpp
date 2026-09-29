#pragma once

#include <string>
#include <vector>

namespace sim {

// Screening of everything that crosses the pipeline in text form. Two calls
// per turn, mirroring the department's own tool: once on the student's words
// before any model sees them, once on the examiner's words before anything is
// spoken or stored. Audio is never screened, which is the reason the audio may
// not go straight to the examiner any more - see docs/compliance/compliant-flow.md.

enum class SafetyStage {
    StudentSpeech,
    //the transcript, after STT and before the examiner call
    ExaminerReply,
    //the generated question, after the examiner call and before the socket,
    //piper and the store
};

enum class SafetyAction {
    Allow,
    Mask,
    //the text may continue with the offending tokens replaced. Student speech
    //only: a beginner swearing at a microphone should cost them the word, not
    //the exam
    Halt,
    //this turn does not continue. The examiner is not called, or its reply is
    //discarded, and the session is told why
    Escalate,
    //Halt, plus a flag the teacher sees and a wellbeing message on screen.
    //Never silent: an escalated turn that looked to the student like a network
    //error is the failure mode Child Safe Standard 8 exists to prevent
};

struct SafetyVerdict {
    SafetyAction action = SafetyAction::Allow;

    std::string text;
    //what the caller should use from here on. Equal to the input for Allow,
    //masked for Mask, and the input untouched for Halt and Escalate - the
    //caller stores it in the safety event and drops it from the exam

    std::string category;
    //"profanity", "jailbreak", "hate", "sexual", "violence", "self_harm",
    //"off_topic", "unavailable". Written to safety_events verbatim, so the
    //strings are part of the record and not free text

    int severity = 0;
    //0-6, the scale Azure Content Safety returns. A wordlist hit reports 2,
    //which is the threshold the chain halts at by default

    std::string detector;
    //"wordlist", "content_safety", "prompt_shield". Which layer fired, so a
    //review can tell a local match from a remote one when the two disagree

    std::vector<std::string> matches;
    //the normalised tokens or phrases that fired, never the surrounding
    //sentence. Enough to tune the lists without copying student speech into
    //a second place it did not need to be
};

class InterfaceSafety {
public:
    virtual ~InterfaceSafety() = default;

    virtual SafetyVerdict screen(const std::string& text,
                                 SafetyStage stage,
                                 const std::string& language_id) = 0;
    //language_id selects the wordlists and tells a remote checker which
    //language to read. An empty string means the server default

    virtual bool available() const { return true; }
    //false once a remote layer has failed its health check. The chain reads
    //this rather than discovering it mid-turn, so a dead Content Safety
    //resource stops new exams starting instead of halting them one turn in

    virtual void prewarm() {}
    //same contract as InterfaceExaminer::prewarm - called once per worker
    //thread at startup, off the critical path
};

const char* to_string(SafetyAction action);
const char* to_string(SafetyStage stage);
//for the log line and the safety_events row

}  // namespace sim
