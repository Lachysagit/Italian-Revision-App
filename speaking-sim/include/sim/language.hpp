#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/config.hpp"
#include "sim/question_bank.hpp"

namespace sim {

// Everything about an exam that varies by language, in one place. Before this
// existed the language was spread across five layers - whisper's language
// parameter, the piper voice, three prompt paths, Italian prose compiled into
// session.cpp and question_bank.cpp, and the translate allowlist - and adding a
// second one meant finding all five. A third language now means one more entry
// in built_in_packs() and one more directory under prompts/.
struct LanguagePack {
    std::string id;
    //the value the browser sends on the Start message, and the directory name
    //under prompts/. Lowercase English, matching listening/'s own directories
    std::string display_name;
    //what the settings picker shows
    std::string translate_code;
    //ISO code for the translate box, which is a different vocabulary from
    //whisper's even where the two happen to agree
    std::string whisper_code;
    //goes to wparams.language
    std::string piper_voice_path;
    std::string prompt_dir;

    std::string opening_turn_text;
    //the user turn that starts the exam. Gemini rejects an empty contents
    //array, and the opening turn's history is the system prompt alone
    std::string prewarm_text;
    //thrown away; only its spawn cost matters

    std::vector<std::string> opinion_openers;
    //lowercase ASCII substrings. asks_opinion() folds case byte by byte with
    //std::tolower, so an opener carrying a non-ASCII letter never matches and
    //the exam silently loses its opinion question. German's natural opener
    //"was haeltst du" is exactly that trap, which is why it is not in the list
    std::string opinion_opener_label;
    //quoted back to the examiner as the phrase to open an opinion question with

    std::string anonymity_sentence;
    //sent as a System turn on every snapshot. It carries NO slot and names no
    //student: this is the turn that used to supply the name and now withholds
    //it. Keep it slotless - a {0} here is an invitation to put the name back
    std::string sample_question_header;
    //"...:" precedes the drawn questions; {0} stands in for the topic group
    std::string sample_question_footer;
    std::string opening_sample_footer;
    //the same, for the opening turn. It cannot share the footer above, which
    //tells the examiner not to copy a sample word for word and to answer what
    //the student just said: on the first question there is nothing to answer,
    //and copying a sample is exactly what is wanted. Handed both at once, the
    //examiner padded the question out to satisfy the contradiction

    std::vector<std::pair<std::string, std::string>> tense_labels;
    //canonical key from tenses.hpp -> the name a teacher of this language uses
    //for it. Shown on the dashboard and handed to the examiner in the schema,
    //so it knows "perfect" means the passato prossimo in an Italian exam
    std::vector<std::pair<std::string, std::string>> tense_examples;
    //canonical key -> a question opening that invites an answer in that tense,
    //quoted to the examiner when a plan's tense target is falling behind

    // Filled by load(), not by the built-in table.
    std::string first_prompt;
    std::string ongoing_prompt;
    std::shared_ptr<const QuestionBank> question_bank;
};

// The packs available to a run, built once at startup and const thereafter, so
// a Session can hold a bare pointer into it for its whole life.
class LanguageRegistry {
public:
    // Never throws and never returns empty: a language whose prompt files are
    // missing still gets a pack carrying the built-in fallback prompts, on the
    // same reasoning as load_prompt() before it. A broken German install must
    // leave Italian working.
    static LanguageRegistry load(const Config& config);

    // nullptr when the id is unknown, so callers fall back deliberately rather
    // than being handed a default they did not ask for.
    const LanguagePack* find(const std::string& id) const;

    const LanguagePack& default_pack() const;

    std::vector<const LanguagePack*> all() const;

private:
    std::map<std::string, LanguagePack> packs_;
    std::string default_id_;
};

// Every voice a run can reach, resolved the same way LanguageRegistry::load
// resolves them. main() hands this to PiperTTS so each voice's sample rate is
// read at startup; keeping both on one function is what stops the TTS cache and
// the registry disagreeing about which file a language plays.
std::vector<std::string> configured_voice_paths(const Config& config);

}  // namespace sim
