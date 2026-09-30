#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sim/safety/adjudicator.hpp"

namespace sim {

// The rules around the backend. This class makes no network call and holds no
// model: it decides what a model is ALLOWED to change, calls the backend only
// when the answer could matter, and clamps whatever comes back.
//
// The sentence to defend in the assessment request:
//
//     adjudication resolves disagreement between detectors;
//     it never overrides consensus.
//
// Everything below is that rule made mechanical.
//
// ---- what may be reviewed --------------------------------------------------
//
//   jailbreak    NEVER. The text being judged is text that just tried to
//                subvert a model, so asking a second model whether to let it
//                through is the attack, not the defence. This exclusion also
//                removes the largest prompt-injection surface at no cost
//   profanity    NEVER. An exact match against a curated token list has no
//                ambiguity to resolve, and a mask is cheap enough not to
//                justify a model call
//   hate         may fall to Mask, never lower. A slur quoted and a slur used
//                are genuinely different, and being wrong is still serious
//   sexual       may fall to Allow. This is where the false positives are:
//   violence     a film plot or a war topic is legitimate exam content that a
//                severity-2 threshold will flag
//   self_harm    tie-break only, and never below Halt. See below
//
// ---- the self-harm rule ----------------------------------------------------
//
// Three gates, all required:
//
//   1. the detectors disagree. concurring_detectors must be 1 - if the local
//      list AND Content Safety both called it self-harm, no model gets a vote
//   2. the matched phrase is not marked non-adjudicable. Entries prefixed "!"
//      in escalate.txt are outside this class's reach whatever else is true
//   3. self_harm is explicitly enabled in the configuration, which it is not
//      by default
//
// Even then the floor is Halt, never Allow or Mask. The worst case of a wrong
// downgrade is a student losing one turn and getting the question refunded;
// the worst case of the opposite is a child's disclosure discarded by a
// language model. Those are not comparable errors and the floor says so.
class SemanticAdjudicator {
public:
    struct Options {
        std::vector<std::string> categories{"sexual", "violence", "hate"};
        //which categories may be reviewed at all. self_harm is governed by the
        //flag below rather than by membership here, so it cannot be switched
        //on by editing a comma-separated list
        bool self_harm = false;
    };

    SemanticAdjudicator(std::unique_ptr<InterfaceAdjudicator> backend,
                        Options options);

    AdjudicationResult review(const std::string& text,
                              const SafetyVerdict& verdict,
                              const std::string& language_id);
    //never throws. A backend that throws, times out or answers unusably comes
    //back as Unavailable with the original action untouched

    bool available() const;
    void prewarm();

    static SafetyAction floor_for(const std::string& category);
    //the lowest action a downgrade may reach for a category. Exposed for the
    //tests, because this is the number a reviewer will ask about

private:
    bool adjudicable(const SafetyVerdict& verdict) const;

    std::unique_ptr<InterfaceAdjudicator> backend_;
    Options options_;
};

}  // namespace sim
