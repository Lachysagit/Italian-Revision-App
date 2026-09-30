#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sim/safety.hpp"

namespace sim {

// The local layer. No network, no key, no cost, and it is the only layer that
// still works on the Pi with the network cable out - which is the whole point
// of keeping it separate from the Azure one rather than folding both into a
// single class.
//
// Three lists per language, loaded from disk on first use:
//   <dir>/<language>/profanity.txt   one term per line, matched per token
//   <dir>/<language>/jailbreak.txt   one phrase per line, matched as substring
//   <dir>/<language>/escalate.txt    one phrase per line, substring, escalates
//
// The lists are deployment data, not source. The live .txt files are not in
// the repository: a school may need to add terms its students actually use,
// and a marking rubric for a language exam is a poor place to keep a list of
// slurs under version control.
//
// A tracked <name>.txt.example sits beside each one and is read when the live
// file is absent, so a fresh clone is screened rather than silently running
// with three empty lists. Three empty lists is the one case that is not
// tolerated: available() returns false for it, and SafetyChain::ready() then
// refuses to let an exam begin. A partly-populated set is fine - the remote
// layer is the one that must be present in the deployed configuration.
class WordlistSafety : public InterfaceSafety {
public:
    explicit WordlistSafety(std::string directory);

    SafetyVerdict screen(const std::string& text,
                         SafetyStage stage,
                         const std::string& language_id) override;

    void prewarm() override;
    //loads every shipped language's lists now, so the first turn does not pay
    //three file reads while a student watches - and so available() has
    //something to answer from before the first exam starts

    bool available() const override;
    //false until at least one list has been loaded with something in it

    struct Phrase {
        std::string text;
        bool non_adjudicable = false;
        //written in the file as a leading "!". The marker is stripped before
        //normalisation, so "!kill myself" and "kill myself" match identically
        //and differ only in whether the semantic pass may touch the verdict
    };
    //public so the file readers can name it: this is the on-disk shape of a
    //phrase list, which is a detail of the format rather than of this class

private:
    struct Lists {
        std::unordered_set<std::string> profanity;
        std::vector<Phrase> jailbreak;
        std::vector<Phrase> escalate;
        bool loaded = false;
    };

    const Lists& lists_for(const std::string& language_id);
    //loads on first ask and caches. Holds m_ for the read, which is a few
    //microseconds once warm and is not on the audio path anyway

    std::string directory_;
    std::mutex m_;
    std::atomic<bool> loaded_something_{false};
    //read by available() without taking m_, which is called from the Start
    //handler on a socket thread while a worker may be loading a new language
    std::unordered_map<std::string, Lists> cache_;
};

// Exposed for the unit tests and for the desk check in the HSC documentation:
// the normaliser is the only interesting part of this class, and a test that
// can only reach it through screen() cannot show what it does.
std::string normalise_for_match(const std::string& text);
//lowercased, accents folded to their base letter, common character
//substitutions undone (0 to o, 3 to e, @ to a), repeated letters collapsed,
//everything that is not a letter or digit turned into a single space

}  // namespace sim
