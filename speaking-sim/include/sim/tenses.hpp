#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sim {

// The tenses the exam tracks, as keys that are the same in every language. A
// language's own names for them (passato prossimo, Perfekt) live in its
// LanguagePack; reports and exam plans store only these keys, which is what
// lets a class report compare an Italian exam with a German one.
//
//   present      presente / Praesens
//   perfect      passato prossimo / Perfekt - the spoken past
//   imperfect    imperfetto / Praeteritum
//   future       futuro semplice / Futur I
//   conditional  condizionale presente / Konjunktiv II
constexpr std::array<std::string_view, 5> kTenseKeys{
    "present", "perfect", "imperfect", "future", "conditional",
};

bool is_tense_key(const std::string& key);

// A deterministic second opinion on which tenses a sentence uses, alongside
// the examiner's own label. Word lists and endings, not a parser: good at the
// forms a beginner produces, blind to anything clever, and never "present",
// which is too ambiguous to call from endings alone. Unknown language ids get
// an empty answer rather than a wrong one.
std::vector<std::string> detect_tenses(const std::string& language_id,
                                       const std::string& text);

// Runs detect_tenses over a fixed table of sentences and prints what it
// found against what it should have. Backs --check-tenses in main.cpp.
int check_tense_rules();

}  // namespace sim
