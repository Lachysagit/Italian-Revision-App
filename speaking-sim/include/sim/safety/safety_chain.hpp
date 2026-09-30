#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "sim/safety.hpp"
#include "sim/safety/semantic_adjudicator.hpp"

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
                Options options,
                std::unique_ptr<SemanticAdjudicator> adjudicator = nullptr);
    //the adjudicator is optional and null is the ordinary configuration. A
    //chain without one behaves exactly as before: every verdict stands

    SafetyVerdict screen(const std::string& text,
                         SafetyStage stage,
                         const std::string& language_id);
    //never throws. A failure becomes a verdict with category "unavailable",
    //so every caller has exactly one shape to handle and the store gets a row
    //for the outage as well as for the hits

    const AdjudicationResult& last_adjudication() const { return last_; }
    //what the semantic pass did to the verdict just returned, for the
    //safety_events row. Valid until the next screen() on this thread

    bool ready() const;
    //every layer's available(). Read by main() at startup and by the Start
    //handler: an exam that cannot be screened does not begin

    void prewarm();

private:
    int count_concurring(const SafetyVerdict& verdict, const std::string& text,
                         SafetyStage stage, const std::string& language_id,
                         std::size_t after);
    //how many layers independently reach the same category. Only ever called
    //for an escalation, which is the one verdict whose reviewability depends
    //on whether the detectors agreed

    SafetyVerdict adjudicated(SafetyVerdict verdict, const SafetyVerdict& carried,
                              const std::string& text, SafetyStage stage,
                              const std::string& language_id);
    //runs the semantic pass, if there is one, and records what it did in last_.
    //`carried` is what the layers before this one had concluded - a cleared
    //verdict falls back to it rather than to Allow, so a mask applied earlier
    //in the chain survives the clearance

    std::vector<std::unique_ptr<InterfaceSafety>> layers_;
    Options options_;
    std::unique_ptr<SemanticAdjudicator> adjudicator_;
    static thread_local AdjudicationResult last_;
    //thread_local rather than a plain member: one chain is shared by every
    //worker, and the result belongs to the turn that asked, not to the chain
};

}  // namespace sim
