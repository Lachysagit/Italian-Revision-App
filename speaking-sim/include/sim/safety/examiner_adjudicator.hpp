#pragma once

#include <atomic>
#include <string>

#include "sim/examiner.hpp"
#include "sim/safety/adjudicator.hpp"

namespace sim {

// The reasoning backend, driven through InterfaceExaminer rather than a client
// of its own. That is not laziness: it means the adjudicator runs on whatever
// the examiner runs on, so when the examiner moves to Azure OpenAI in
// Australia East, this moves with it and cannot be left pointing at a
// processor the compliance case does not cover.
//
// The corollary is a startup check rather than a comment - load_config()
// refuses SAFETY_ADJUDICATOR=examiner with AUTH_REQUIRED on while the examiner
// backend is Gemini, because flagged student speech is the most sensitive text
// this system handles and offshore is exactly where it must not go.
//
// ---- the injection problem -------------------------------------------------
//
// Every string this class is given is attacker-controlled by construction: it
// is text that a safety layer just objected to. Four things follow, and all
// four are load-bearing:
//
//   1. jailbreak verdicts never reach here at all. SemanticAdjudicator drops
//      them before a backend is called, so the one category whose whole
//      purpose is subverting a model is never shown to one
//   2. the utterance travels as a JSON string value in a user turn, never
//      interpolated into the system prompt
//   3. the reply must parse to a fixed three-field schema. Prose, refusals,
//      extra fields and missing fields all read as "uphold"
//   4. there is no free-text field in the schema, so nothing the model writes
//      can reach the database or a teacher's screen
class ExaminerAdjudicator : public InterfaceAdjudicator {
public:
    ExaminerAdjudicator(InterfaceExaminer* examiner, std::string key_name);
    //borrows the examiner rather than owning it: Server already owns one, and
    //a second instance would mean a second connection pool for a call that
    //happens a handful of times a term

    AdjudicatorOpinion review(const std::string& text,
                              const std::string& category,
                              const std::string& language_id) override;

    bool available() const override;
    void prewarm() override;

    static AdjudicatorOpinion parse(const std::string& reply);
    //exposed for the tests. Every rejection path returns the upholding
    //default, so a test can assert that garbage never clears a verdict

private:
    InterfaceExaminer* examiner_;
    std::string key_name_;
    std::atomic<bool> healthy_{false};
};

std::string adjudication_system_prompt(const std::string& category);
//built per category so the model is asked one narrow question rather than a
//general one. Exposed so the exact wording can be quoted in the PIA without
//anyone having to read it out of a string literal

}  // namespace sim
