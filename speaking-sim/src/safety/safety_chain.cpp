#include "sim/safety/safety_chain.hpp"

#include <algorithm>
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

}  // namespace

SafetyChain::SafetyChain(std::vector<std::unique_ptr<InterfaceSafety>> layers,
                         Options options)
    : layers_(std::move(layers)), options_(options) {}

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
}

SafetyVerdict SafetyChain::screen(const std::string& text,
                                  SafetyStage stage,
                                  const std::string& language_id) {
    SafetyVerdict carried;
    carried.text = text;
    carried.detector = "chain";

    for (auto& layer : layers_) {
        try {
            SafetyVerdict verdict = layer->screen(carried.text, stage, language_id);

            if (stops_the_turn(verdict.action)) return verdict;

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
    //Allow with the untouched text, or Mask with the accumulated masked copy
}

}  // namespace sim
