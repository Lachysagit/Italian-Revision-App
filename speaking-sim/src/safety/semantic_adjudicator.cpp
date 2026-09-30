#include "sim/safety/semantic_adjudicator.hpp"

#include <algorithm>
#include <iostream>

namespace sim {

namespace {

constexpr const char* kReasonCodes[] = {
    "genuine", "idiomatic", "quoted", "fictional", "historical", "ambiguous",
};

int rank(SafetyAction action) {
    switch (action) {
        case SafetyAction::Allow:    return 0;
        case SafetyAction::Mask:     return 1;
        case SafetyAction::Halt:     return 2;
        case SafetyAction::Escalate: return 3;
    }
    return 3;
    //an unknown action ranks as the most severe, so a future enum value added
    //without touching this switch cannot be silently downgraded
}

}  // namespace

bool is_reason_code(const std::string& code) {
    return std::find(std::begin(kReasonCodes), std::end(kReasonCodes), code) !=
           std::end(kReasonCodes);
}

bool reason_permits_downgrade(const std::string& code) {
    return code == "idiomatic" || code == "quoted" || code == "fictional" ||
           code == "historical";
    //"genuine" is the backend agreeing with the layer. "ambiguous" is the
    //backend saying it cannot tell, which is an uphold rather than a pass:
    //the benefit of the doubt belongs to the student's safety, not to the
    //smoothness of their exam
}

const char* to_string(AdjudicationOutcome outcome) {
    switch (outcome) {
        case AdjudicationOutcome::NotAdjudicable: return "not_adjudicable";
        case AdjudicationOutcome::Upheld:         return "upheld";
        case AdjudicationOutcome::Downgraded:     return "downgraded";
        case AdjudicationOutcome::Unavailable:    return "unavailable";
    }
    return "upheld";
}

SemanticAdjudicator::SemanticAdjudicator(
    std::unique_ptr<InterfaceAdjudicator> backend, Options options)
    : backend_(std::move(backend)), options_(std::move(options)) {}

bool SemanticAdjudicator::available() const {
    return backend_ && backend_->available();
}

void SemanticAdjudicator::prewarm() {
    if (backend_) backend_->prewarm();
}

void SemanticAdjudicator::set_detector_count(int detectors) {
    options_.detectors = detectors;
}

SafetyAction SemanticAdjudicator::floor_for(const std::string& category) {
    if (category == "self_harm") return SafetyAction::Halt;
    return SafetyAction::Allow;

    // hate used to floor at Mask, on the reasoning that a slur quoted and a
    // slur used are different and the middle ground was worth keeping. That
    // floor was unimplementable and quietly dishonest: Mask means "replace
    // these tokens", Content Safety reports a category and a severity but no
    // spans, so a Halt cleared to Mask produced a verdict that SAID a word was
    // hidden while the text went through verbatim.
    //
    // Clearing it outright is safe for a reason that is worth stating: the
    // actual slurs are in profanity.txt, the wordlist layer masks them, and
    // that layer is non-adjudicable. So a listed slur is masked whatever the
    // reasoning pass concludes about the sentence around it, and the chain now
    // carries that mask through a clearance. What "hate" adds on top is a
    // judgement about MEANING, and meaning is exactly what the reasoning pass
    // is better placed to judge than a severity threshold - "we studied the
    // White Australia policy" is a history answer.
    //
    // The general principle, which this follows: prefer letting text through.
    // An exam stopped over a legitimate answer is a cost paid by every student
    // who phrases something awkwardly.
}

bool SemanticAdjudicator::adjudicable(const SafetyVerdict& verdict) const {
    if (verdict.action == SafetyAction::Allow) return false;

    if (verdict.non_adjudicable) return false;
    //an escalate.txt entry prefixed "!" - the unambiguous phrasings, which no
    //model may talk the server out of whatever else is true

    const std::string& category = verdict.category;

    if (category == "jailbreak") return false;
    if (category == "profanity") return false;
    if (category == "unavailable") return false;
    //a chain outage is not a semantic question, and reviewing it would mean
    //asking a model to overrule a fail-closed policy

    if (category == "self_harm") {
        if (!options_.self_harm) return false;

        if (options_.detectors < 2) return false;
        //there is no second opinion to weigh against the first, so there is no
        //disagreement to settle. A lone detector's escalation is the only
        //judgement that exists, and giving a model the casting vote on it is
        //not a tie-break - it is a veto. This is the gate that makes
        //SAFETY_MODE=local behave the way the documentation says it does

        if (verdict.concurring_detectors > 1) return false;
        //consensus is untouchable. Two independent detectors calling the same
        //utterance self-harm is not a disagreement for a third opinion to
        //settle

        return true;
    }

    return std::find(options_.categories.begin(), options_.categories.end(),
                     category) != options_.categories.end();
}

AdjudicationResult SemanticAdjudicator::review(const std::string& text,
                                               const SafetyVerdict& verdict,
                                               const std::string& language_id) {
    AdjudicationResult result;
    result.original_action = verdict.action;
    result.action = verdict.action;

    if (!adjudicable(verdict)) {
        result.outcome = AdjudicationOutcome::NotAdjudicable;
        return result;
        //no backend call is made at all, which is the point: the categories
        //above never leave the server, so there is nothing for an injected
        //instruction inside them to reach
    }

    if (!available()) {
        result.outcome = AdjudicationOutcome::Unavailable;
        return result;
    }

    AdjudicatorOpinion opinion;
    try {
        opinion = backend_->review(text, verdict.category, language_id);
    } catch (const std::exception& e) {
        std::cerr << "adjudicator failed on " << verdict.category << ": "
                  << e.what() << '\n';
        result.outcome = AdjudicationOutcome::Unavailable;
        return result;
    }

    result.reason_code =
        is_reason_code(opinion.reason_code) ? opinion.reason_code : "";

    // Four independent reasons to uphold, checked separately rather than as
    // one condition so that the log and the stored row can say which applied.
    if (opinion.genuine) {
        result.outcome = AdjudicationOutcome::Upheld;
        return result;
    }
    if (opinion.confidence != "high") {
        result.outcome = AdjudicationOutcome::Upheld;
        return result;
    }
    if (result.reason_code.empty()) {
        result.outcome = AdjudicationOutcome::Upheld;
        return result;
        //an unrecognised code means the backend did not answer the question
        //that was asked, which is not evidence of anything
    }
    if (!reason_permits_downgrade(result.reason_code)) {
        result.outcome = AdjudicationOutcome::Upheld;
        return result;
    }

    const SafetyAction floor = floor_for(verdict.category);
    if (rank(floor) >= rank(verdict.action)) {
        result.outcome = AdjudicationOutcome::Upheld;
        return result;
        //the floor is already at or above where the verdict sits, so there is
        //nothing a downgrade could do. Recorded as Upheld rather than
        //Downgraded because nothing changed
    }

    result.outcome = AdjudicationOutcome::Downgraded;
    result.action = floor;
    return result;
    //one code path reduces a verdict, and reaching it takes an affirmative,
    //well-formed, high-confidence answer with a reason that permits it, about
    //a category that allows it, on a verdict no other detector concurred with
}

}  // namespace sim
