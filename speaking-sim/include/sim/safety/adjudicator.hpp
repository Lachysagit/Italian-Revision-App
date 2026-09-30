#pragma once

#include <string>
#include <vector>

#include "sim/safety.hpp"

namespace sim {

// The semantic reasoning pass. Where the chain's layers decide WHETHER a turn
// trips a rule, this decides whether the trip was GENUINE - "mi piace da
// morire" is an idiom, "voglio morire" is not, and no wordlist or severity
// score can tell those apart.
//
// NSWEduChat runs a semantic content filter that "checks the meaning behind
// your words", so a meaning-level stage is not an invention here. What IS an
// extension is letting that stage REDUCE another layer's verdict, and the
// whole design of this file is about bounding that power:
//
//   * it only ever sees text a layer has already flagged, so no extra student
//     speech is disclosed to a model that would not have been anyway;
//   * it can only lower a verdict, never raise one;
//   * several categories are outside its reach entirely;
//   * every failure mode upholds.
//
// It is NOT an InterfaceSafety. A safety layer screens text and produces a
// verdict; this reviews a verdict that already exists. Folding it into the
// chain as another layer would blur exactly the distinction a reviewer will
// want to see kept.

// What a backend is allowed to say. Deliberately tiny and entirely enumerated:
// there is no free-text field anywhere in this struct, because a model's prose
// explanation of why an utterance was benign is a paraphrase of that utterance,
// and it would end up in the one table whose design principle is that it holds
// no sentences.
struct AdjudicatorOpinion {
    bool genuine = true;
    //true means the trigger was real and the verdict stands. The default is
    //the safe answer, so a partially-filled struct upholds

    std::string confidence;
    //"high", "medium" or "low". Anything but "high" upholds

    std::string reason_code;
    //one of the codes below. An unrecognised code upholds
};

bool is_reason_code(const std::string& code);
//genuine, idiomatic, quoted, fictional, historical, ambiguous

bool reason_permits_downgrade(const std::string& code);
//"genuine" and "ambiguous" never do, whatever the backend said about its own
//confidence: an adjudicator that is unsure is an adjudicator that upholds

class InterfaceAdjudicator {
public:
    virtual ~InterfaceAdjudicator() = default;

    virtual AdjudicatorOpinion review(const std::string& text,
                                      const std::string& category,
                                      const std::string& language_id) = 0;
    //may throw. Every throw is an uphold, so a backend does not need to know
    //the policy in order to fail safely

    virtual bool available() const { return false; }
    //false by default: an adjudicator that has not proven itself must not be
    //trusted to reduce anything

    virtual void prewarm() {}
};

// ---- the policy -------------------------------------------------------------

enum class AdjudicationOutcome {
    NotAdjudicable,
    //the category, the entry or the configuration put this verdict out of
    //reach. No backend call was made
    Upheld,
    Downgraded,
    Unavailable,
    //the backend failed, timed out or answered unusably. Upholds, and is
    //recorded distinctly from Upheld so an outage is visible in the audit
};

const char* to_string(AdjudicationOutcome outcome);

struct AdjudicationResult {
    AdjudicationOutcome outcome = AdjudicationOutcome::NotAdjudicable;

    SafetyAction original_action = SafetyAction::Allow;
    //what the chain decided before review. Kept because the chain applies
    //`action` to the verdict it returns, so without this the pre-review
    //severity would be unrecoverable by the time the row is written - and
    //"what did the filters stop" is the number an audit actually wants

    SafetyAction action = SafetyAction::Allow;
    //the action to apply from here on. Equal to original_action for every
    //outcome except Downgraded

    std::string reason_code;
};

}  // namespace sim
