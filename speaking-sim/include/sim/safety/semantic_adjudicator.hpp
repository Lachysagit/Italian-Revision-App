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
// Four gates, all required:
//
//   1. corroboration is POSSIBLE at all: at least two detectors are configured.
//      One detector cannot disagree with itself, so a lone wordlist hit is not
//      a tie for anything to break - it is the only opinion there is, and
//      handing a model the deciding vote on the only opinion there is would be
//      the opposite of a tie-break. This is what makes the rule mean the same
//      thing in SAFETY_MODE=local as it does in azure
//   2. the detectors actually disagree. concurring_detectors must be 1 - if the
//      local list AND Content Safety both called it self-harm, no model gets a
//      vote
//   3. the matched phrase is not marked non-adjudicable. Entries prefixed "!"
//      in escalate.txt are outside this class's reach whatever else is true
//   4. self_harm is explicitly enabled in the configuration, which it is not
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

        int detectors = 0;
        //how many independent screening layers the chain holds. Set by
        //SafetyChain at construction, because the chain is what knows. Gate 1
        //above reads it: below two, self-harm review cannot run at all, so the
        //offline build is not quietly operating a rule it has no second
        //opinion to apply
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

    void set_detector_count(int detectors);
    //called by SafetyChain once, at construction. Separate from Options so the
    //count comes from the chain that actually holds the layers rather than
    //from a configuration value someone could set wrongly

    static SafetyAction floor_for(const std::string& category);
    //the lowest action a downgrade may reach for a category. Exposed for the
    //tests, because this is the number a reviewer will ask about

private:
    bool adjudicable(const SafetyVerdict& verdict) const;

    std::unique_ptr<InterfaceAdjudicator> backend_;
    Options options_;
};

}  // namespace sim
