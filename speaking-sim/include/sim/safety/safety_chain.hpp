#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sim/safety.hpp"

namespace sim {

// Runs the layers in order and stops at the first one that does not allow.
// Order is deliberate and is the cheap-and-local layer first: a word on the
// list costs no network hop, and on the Hailo build there is no second layer
// to reach.
//
// Failure policy, which is the part a reviewer will ask about:
//
//   ExaminerReply  a layer that throws means Halt. Unscreened generated text
//                  has never been spoken to a student and never will be
//   StudentSpeech  a layer that throws means Halt as well, when fail_closed is
//                  on. The wordlist layer has already run by then, so the
//                  student is not unscreened - but "the filter was down and we
//                  carried on anyway" is exactly the sentence that loses an
//                  assessment, so the default is to stop
//
// fail_closed is configurable so the offline Pi build, which has no remote
// layer at all, is not describing a policy it cannot execute.
class SafetyChain {
public:
    struct Options {
        bool fail_closed = true;
    };

    SafetyChain(std::vector<std::unique_ptr<InterfaceSafety>> layers,
                Options options);

    SafetyVerdict screen(const std::string& text,
                         SafetyStage stage,
                         const std::string& language_id);
    //never throws. A failure becomes a verdict with category "unavailable",
    //so every caller has exactly one shape to handle and the store gets a row
    //for the outage as well as for the hits

    bool ready() const;
    //every layer's available(). Read by main() at startup and by the Start
    //handler: an exam that cannot be screened does not begin

    void prewarm();

private:
    std::vector<std::unique_ptr<InterfaceSafety>> layers_;
    Options options_;
};

}  // namespace sim
