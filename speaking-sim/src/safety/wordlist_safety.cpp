#include "sim/safety/wordlist_safety.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iostream>

namespace sim {

namespace {

constexpr const char* kMask = "***";

// Wordlist hits report the same severity the remote layer's threshold sits at,
// so one number in the config governs both and the two layers cannot disagree
// about what counts as bad enough to stop.
constexpr int kWordlistSeverity = 2;

// The accented letters Italian, German and English actually produce, folded to
// the base letter so "perchè" and "perche" are one token. UTF-8 two-byte
// sequences, matched on the pair rather than decoded: the alternative is a
// dependency on ICU for a table this size.
struct Fold {
    const char* utf8;
    char base;
};

constexpr Fold kFolds[] = {
    {"à", 'a'}, {"á", 'a'}, {"â", 'a'}, {"ä", 'a'}, {"ã", 'a'}, {"å", 'a'},
    {"è", 'e'}, {"é", 'e'}, {"ê", 'e'}, {"ë", 'e'},
    {"ì", 'i'}, {"í", 'i'}, {"î", 'i'}, {"ï", 'i'},
    {"ò", 'o'}, {"ó", 'o'}, {"ô", 'o'}, {"ö", 'o'}, {"õ", 'o'},
    {"ù", 'u'}, {"ú", 'u'}, {"û", 'u'}, {"ü", 'u'},
    {"ç", 'c'}, {"ñ", 'n'}, {"ß", 's'},
};

// Undone before matching, so a student typing round the filter has to get
// further from the word than the filter is from the word.
char unleet(char ch) {
    switch (ch) {
        case '0': return 'o';
        case '1': return 'i';
        case '3': return 'e';
        case '4': return 'a';
        case '5': return 's';
        case '7': return 't';
        case '@': return 'a';
        case '$': return 's';
        default:  return ch;
    }
}

bool is_word_char(char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return std::isalnum(c) != 0;
}

// Terms, for the profanity list: no "!" marker, because a token match against
// a curated list is never adjudicable in the first place.
bool read_terms_into(const std::string& path, std::vector<std::string>& out) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::string normalised = normalise_for_match(line);
        if (!normalised.empty()) out.push_back(normalised);
    }
    return true;
}

// Phrases, for the jailbreak and escalation lists. A leading "!" marks the
// entry non-adjudicable: the semantic reasoning pass may not reduce a verdict
// this phrase produced. The marker is stripped before normalising, so marking
// an entry never changes what it matches - only what may be done about it.
bool read_phrases_into(const std::string& path,
                       std::vector<WordlistSafety::Phrase>& out) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        WordlistSafety::Phrase phrase;
        std::string body = line;
        if (!body.empty() && body[0] == '!') {
            phrase.non_adjudicable = true;
            body.erase(0, 1);
        }
        phrase.text = normalise_for_match(body);
        if (!phrase.text.empty()) out.push_back(std::move(phrase));
    }
    return true;
}

// The live file wins; the tracked .example beside it is the fallback. A clone
// that has never had a wordlist directory is still screened, rather than
// running with three empty lists and a ready() that says yes anyway.
template <typename Out, typename Read>
bool load_list(const std::string& base, const char* name, Out& out, Read read) {
    const std::string path = base + name + ".txt";
    if (read(path, out)) return true;

    if (read(path + ".example", out)) {
        std::cerr << "safety: " << path << " not found, using the tracked "
                  << name << ".txt.example seed list\n";
        return true;
    }
    return false;
}

}  // namespace

// Four passes in one loop rather than four string copies: this runs on every
// transcript and every reply, twice per turn, on a worker that has other work.
std::string normalise_for_match(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    char previous = '\0';
    bool pending_space = false;

    for (std::size_t i = 0; i < text.size();) {
        char ch = '\0';
        std::size_t width = 1;

        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead >= 0x80) {
            bool folded = false;
            for (const Fold& fold : kFolds) {
                const std::size_t length = std::char_traits<char>::length(fold.utf8);
                if (text.compare(i, length, fold.utf8) == 0) {
                    ch = fold.base;
                    width = length;
                    folded = true;
                    break;
                }
            }
            if (!folded) {
                //an unfolded multibyte character is a separator rather than a
                //letter: it cannot be part of a list entry, which are ASCII
                //once normalised
                width = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
                i += width;
                pending_space = !out.empty();
                previous = '\0';
                continue;
            }
        } else {
            ch = static_cast<char>(std::tolower(lead));
        }

        ch = unleet(ch);

        if (!is_word_char(ch)) {
            i += width;
            pending_space = !out.empty();
            previous = '\0';
            continue;
        }

        if (ch == previous) {
            i += width;
            continue;
            //"sheeeesh" and "sheesh" collapse to the same token. Italian
            //doubles letters meaningfully, so the list entries are normalised
            //through this same function and collapse with them
        }

        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(ch);
        previous = ch;
        i += width;
    }

    return out;
}

WordlistSafety::WordlistSafety(std::string directory)
    : directory_(std::move(directory)) {}

const WordlistSafety::Lists& WordlistSafety::lists_for(
    const std::string& language_id) {
    std::lock_guard<std::mutex> lock(m_);

    Lists& lists = cache_[language_id];
    if (lists.loaded) return lists;
    lists.loaded = true;

    const std::string base = directory_ + "/" + language_id + "/";

    std::vector<std::string> profanity;
    if (!load_list(base, "profanity", profanity, read_terms_into)) {
        std::cerr << "safety: no profanity list for " << language_id
                  << " at " << base << "profanity.txt\n";
    }
    for (std::string& term : profanity) lists.profanity.insert(std::move(term));

    if (!load_list(base, "jailbreak", lists.jailbreak, read_phrases_into)) {
        std::cerr << "safety: no jailbreak list for " << language_id << '\n';
    }
    if (!load_list(base, "escalate", lists.escalate, read_phrases_into)) {
        std::cerr << "safety: no escalation list for " << language_id << '\n';
    }

    if (!lists.profanity.empty() || !lists.jailbreak.empty() ||
        !lists.escalate.empty()) {
        loaded_something_.store(true);
    }

    return lists;
}

void WordlistSafety::prewarm() {
    for (const char* language : {"italian", "german"}) lists_for(language);
    //every language pack the server ships, not just the default: available()
    //is read at startup to decide whether an exam may begin at all, and it
    //cannot answer for a language whose lists it has not tried to open
}

bool WordlistSafety::available() const {
    return loaded_something_.load();
    //three empty lists is not a working local layer. Saying so here is what
    //stops SafetyChain::ready() reporting a screened build on a machine where
    //the wordlist directory was never deployed
}

SafetyVerdict WordlistSafety::screen(const std::string& text,
                                     SafetyStage stage,
                                     const std::string& language_id) {
    SafetyVerdict verdict;
    verdict.text = text;
    verdict.detector = "wordlist";
    if (text.empty()) return verdict;

    const Lists& lists = lists_for(language_id.empty() ? "italian" : language_id);
    const std::string normalised = normalise_for_match(text);

    // Escalation first, then jailbreak, then profanity: a turn can trip more
    // than one, and the one that gets recorded should be the one that decided
    // what happened rather than whichever was checked first.
    for (const Phrase& phrase : lists.escalate) {
        if (normalised.find(phrase.text) != std::string::npos) {
            verdict.action = SafetyAction::Escalate;
            verdict.category = "self_harm";
            verdict.severity = kWordlistSeverity;
            verdict.matches.push_back(phrase.text);
            verdict.non_adjudicable = phrase.non_adjudicable;
            verdict.concurring_detectors = 1;
            return verdict;
        }
    }

    for (const Phrase& phrase : lists.jailbreak) {
        if (normalised.find(phrase.text) != std::string::npos) {
            verdict.action = SafetyAction::Halt;
            verdict.category = "jailbreak";
            verdict.severity = kWordlistSeverity;
            verdict.matches.push_back(phrase.text);
            verdict.non_adjudicable = true;
            verdict.concurring_detectors = 1;
            return verdict;
            //jailbreak is never adjudicable whatever the file says, so the
            //flag is set here rather than read from the entry
        }
    }

    // Masking works on the original text's own whitespace split, not on the
    // normalised copy: the student has to be able to read their transcript
    // back, so capitals, accents and punctuation survive everywhere the filter
    // did not fire. Where it did fire the whole whitespace-delimited token is
    // replaced, trailing punctuation included - "cazzo!" becomes "***", not
    // "***!". That is deliberate: reconstructing which trailing characters
    // were "really" part of the word is guesswork, and guessing wrong in the
    // direction of leaving characters behind is the direction that leaks.
    std::vector<std::string> hits;
    std::string masked;
    masked.reserve(text.size());

    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = text.find_first_not_of(" \t\n\r", i);
        if (start == std::string::npos) {
            masked.append(text, i, std::string::npos);
            break;
        }
        masked.append(text, i, start - i);

        std::size_t end = text.find_first_of(" \t\n\r", start);
        if (end == std::string::npos) end = text.size();

        const std::string word = text.substr(start, end - start);
        const std::string key = normalise_for_match(word);

        if (!key.empty() && lists.profanity.count(key) != 0) {
            masked += kMask;
            hits.push_back(key);
        } else {
            masked += word;
        }
        i = end;
    }

    if (hits.empty()) return verdict;

    verdict.category = "profanity";
    verdict.severity = kWordlistSeverity;
    verdict.matches = std::move(hits);
    verdict.non_adjudicable = true;
    verdict.concurring_detectors = 1;
    //an exact match against a curated token list has no ambiguity for a model
    //to resolve

    // The asymmetry that matters. A student swearing loses the word; an
    // examiner that swears loses the turn, because a generated question is
    // regenerable and a student's answer is not.
    if (stage == SafetyStage::StudentSpeech) {
        verdict.action = SafetyAction::Mask;
        verdict.text = std::move(masked);
    } else {
        verdict.action = SafetyAction::Halt;
    }
    return verdict;
}

const char* to_string(SafetyAction action) {
    switch (action) {
        case SafetyAction::Allow:    return "allow";
        case SafetyAction::Mask:     return "mask";
        case SafetyAction::Halt:     return "halt";
        case SafetyAction::Escalate: return "escalate";
    }
    return "allow";
}

const char* to_string(SafetyStage stage) {
    return stage == SafetyStage::StudentSpeech ? "student_speech"
                                               : "examiner_reply";
}

}  // namespace sim
