#include "sim/safety/safety_chain.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <utility>

namespace sim {

namespace {

// Halt and Escalate end the turn, so the layers after them have nothing left to
// decide and are skipped. Mask does not: the turn continues, so every remaining
// layer still has to see it.
bool stops_the_turn(SafetyAction action) {
    return action == SafetyAction::Halt || action == SafetyAction::Escalate;
}

int rank(SafetyAction action) {
    switch (action) {
        case SafetyAction::Allow:    return 0;
        case SafetyAction::Mask:     return 1;
        case SafetyAction::Halt:     return 2;
        case SafetyAction::Escalate: return 3;
    }
    return 3;
    //an unknown action ranks most severe, so a future enum value cannot be
    //quietly treated as harmless
}

}  // namespace

thread_local AdjudicationResult SafetyChain::last_;

SafetyChain::SafetyChain(std::vector<std::unique_ptr<InterfaceSafety>> layers,
                         Options options,
                         std::unique_ptr<SemanticAdjudicator> adjudicator)
    : layers_(std::move(layers)),
      options_(options),
      adjudicator_(std::move(adjudicator)) {
    if (adjudicator_) {
        adjudicator_->set_detector_count(static_cast<int>(layers_.size()));
        //the chain is what knows how many detectors exist, and the self-harm
        //gate needs that number: below two there is no second opinion, so
        //there is no disagreement for a reasoning pass to settle. Told here
        //rather than configured, so the two can never drift apart
    }
}

bool SafetyChain::ready() const {
    for (const auto& layer : layers_) {
        if (!layer->available()) return false;
    }
    return !layers_.empty();
    //an empty chain is not "nothing to fail", it is an unscreened build. The
    //Pi build still installs the wordlist layer, so empty means misconfigured
}

void SafetyChain::prewarm() {
    for (auto& layer : layers_) layer->prewarm();
    if (adjudicator_) adjudicator_->prewarm();
}

SafetyVerdict SafetyChain::screen(const std::string& text,
                                  SafetyStage stage,
                                  const std::string& language_id) {
    last_ = AdjudicationResult{};
    //every exit from this function must leave last_ describing THIS call.
    //Only adjudicated() writes it, and only a turn-stopping verdict reaches
    //adjudicated(), so without this reset an Allow or a Mask would carry the
    //PREVIOUS screen()'s review out to record_safety - and the guard there is
    //"Allow and not downgraded", so a stale Downgraded writes a safety_events
    //row for a turn on which nothing fired. The examiner-reply checkpoint hits
    //that on the very same turn as any cleared student verdict, and the worker
    //threads are long-lived, so the stale value would otherwise outlive the
    //attempt that produced it

    SafetyVerdict carried;
    carried.text = text;
    carried.detector = "chain";

    std::size_t index = 0;
    for (auto& layer : layers_) {
        const std::size_t here = index++;
        try {
            SafetyVerdict verdict = layer->screen(carried.text, stage, language_id);

            if (verdict.action == SafetyAction::Escalate) {
                // The one verdict the loop does NOT return on immediately.
                // Whether a semantic pass may review an escalation depends on
                // whether the other detectors agreed, so the remaining layers
                // are asked before anything is decided. An escalation is rare
                // enough that the extra call is affordable, and it is the one
                // verdict where being wrong matters most.
                verdict.concurring_detectors =
                    count_concurring(verdict, text, stage, language_id, here);
                //`here`, not `index`: index has already moved past this layer,
                //and passing it would skip the very next layer's opinion
                return adjudicated(verdict, carried, text, stage, language_id);
            }
            if (stops_the_turn(verdict.action)) {
                if (verdict.concurring_detectors == 0) {
                    verdict.concurring_detectors = 1;
                }
                return adjudicated(verdict, carried, text, stage, language_id);
            }

            if (verdict.action == SafetyAction::Mask) {
                // The masked copy is what the next layer reads. A remote
                // checker should never be handed words a local list already
                // decided to remove, and the layers after this one are still
                // owed a look: one sentence can carry a swear word and a
                // disclosure at once, and returning here would let the mask
                // hide the disclosure. That is the whole reason this loop does
                // not simply stop at the first verdict that is not Allow.
                std::string masked = std::move(verdict.text);

                if (carried.action == SafetyAction::Mask) {
                    carried.matches.insert(carried.matches.end(),
                                           verdict.matches.begin(),
                                           verdict.matches.end());
                    carried.severity = std::max(carried.severity, verdict.severity);
                    //two layers masking the same utterance is one event with
                    //two sets of terms, not two events
                } else {
                    carried = std::move(verdict);
                    //only .text was moved out of verdict above; the category,
                    //severity, detector and matches are still its own
                }
                carried.text = std::move(masked);
            }
        } catch (const std::exception& e) {
            std::cerr << "safety layer failed on " << to_string(stage) << ": "
                      << e.what() << '\n';

            SafetyVerdict verdict;
            verdict.text = text;
            verdict.category = "unavailable";
            verdict.detector = "chain";
            verdict.action = (options_.fail_closed ||
                              stage == SafetyStage::ExaminerReply)
                                 ? SafetyAction::Halt
                                 : SafetyAction::Allow;
            return verdict;
            //the exception text goes to the operator's log and the student
            //gets the fixed failure string the rest of the pipeline already
            //uses, which is the same split the examiner and translate paths
            //make. The unmasked original is returned deliberately: an Allow
            //here means the screening did not complete, and pretending a
            //partial mask is a clean result would be the worse lie
        }
    }

    return carried;
    //Allow with the untouched text, or Mask with the accumulated masked copy.
    //Neither is adjudicated: a mask is not reviewable, and an allow has
    //nothing to review
}

// Asks the layers AFTER the one that escalated whether they reach the same
// category. Their verdicts are thrown away apart from the count - this is
// about agreement, not about a better answer. A layer that throws here is not
// an outage for the turn: the escalation already stands, and a missing second
// opinion simply leaves the count at one, which is the conservative reading
// for every caller except the adjudicator's own gate.
int SafetyChain::count_concurring(const SafetyVerdict& verdict,
                                  const std::string& text, SafetyStage stage,
                                  const std::string& language_id,
                                  std::size_t after) {
    int concurring = 1;
    for (std::size_t i = after + 1; i < layers_.size(); ++i) {
        try {
            const SafetyVerdict second =
                layers_[i]->screen(text, stage, language_id);
            if (second.action != SafetyAction::Allow &&
                second.category == verdict.category) {
                ++concurring;
            }
        } catch (const std::exception& e) {
            std::cerr << "safety: second opinion unavailable on "
                      << verdict.category << ": " << e.what() << '\n';
        }
    }
    return concurring;
}

SafetyVerdict SafetyChain::adjudicated(SafetyVerdict verdict,
                                       const SafetyVerdict& carried,
                                       const std::string& text,
                                       SafetyStage stage,
                                       const std::string& language_id) {
    last_ = AdjudicationResult{};
    last_.original_action = verdict.action;
    last_.action = verdict.action;

    if (!adjudicator_ || stage != SafetyStage::StudentSpeech) {
        return verdict;
        //an examiner reply is never adjudicated. A generated question is
        //regenerable, so upholding and regenerating is strictly cheaper than
        //reasoning about whether the model meant it
    }

    last_ = adjudicator_->review(text, verdict, language_id);
    if (last_.outcome != AdjudicationOutcome::Downgraded) return verdict;

    // Cleared. What should continue is not "nothing was wrong" but "everything
    // the OTHER layers concluded, minus the one just cleared" - and the other
    // layers may already have masked a word.
    //
    // The case that drove this: the wordlist masks a swear word, Content Safety
    // then halts the masked copy on violence, and the reasoning pass clears the
    // violence. The turn must continue, and it must continue with the MASK
    // STILL APPLIED. Taking verdict.text alone would be correct; taking the
    // carried state is correct and also keeps the mask's action, so the stored
    // row does not claim the turn passed clean.
    verdict.text = carried.text;
    verdict.action = rank(carried.action) > rank(last_.action) ? carried.action
                                                              : last_.action;
    return verdict;
    //the verdict keeps its category, severity, detector and matches from the
    //layer that stopped it, so an audit can still count clear rates per
    //category. last_ carries what the reasoning pass did to it
}

}  // namespace sim
