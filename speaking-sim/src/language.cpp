#include "sim/language.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

namespace sim {

namespace {

constexpr const char* kDefaultLanguageId = "italian";

// Moved here from Server::load_prompt. The fallback is deliberately one bare
// line: it keeps a server with no prompt files on disk running an exam rather
// than refusing to start, and the log line is the only clue anyone gets.
std::string load_prompt(const std::string& path, const std::string& fallback) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "prompt file " << path
                  << " not found, falling back to a built-in one line prompt"
                  << std::endl;
        return fallback;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// prompts/<id>/<name>, with prompts/<name> tried after it for italian alone.
// The Italian files lived at the root before German arrived, so this keeps a
// checkout that has not moved them running. Delete the fallback once the move
// has settled.
std::string prompt_path(const LanguagePack& pack, const std::string& name) {
    const std::string preferred = pack.prompt_dir + "/" + name;
    std::ifstream probe(preferred);
    if (probe) {
        return preferred;
    }
    if (pack.id == kDefaultLanguageId) {
        const std::string legacy = "prompts/" + name;
        std::ifstream legacy_probe(legacy);
        if (legacy_probe) {
            std::cerr << "using legacy prompt path " << legacy << "; move it to "
                      << preferred << std::endl;
            return legacy;
        }
    }
    return preferred;
    //the preferred path is returned even when it does not exist, so the log
    //line load_prompt prints names where the file was actually wanted
}

std::vector<LanguagePack> built_in_packs() {
    LanguagePack italian;
    italian.id = "italian";
    italian.display_name = "Italian";
    italian.translate_code = "it";
    italian.whisper_code = "it";
    italian.piper_voice_path = "models/it_IT-riccardo-x_low.onnx";
    italian.prompt_dir = "prompts/italian";
    italian.opening_turn_text = "Inizia l'esame.";
    italian.prewarm_text = "Buongiorno.";
    italian.opinion_openers = {"secondo te", "cosa ne pensi", "che ne pensi",
                               "sei d'accordo"};
    italian.opinion_opener_label = "Secondo te";
    italian.student_name_sentence =
        "Lo studente si chiama {0}. Sai gia come si chiama, quindi non "
        "chiedere mai il suo nome.";
    italian.sample_question_header = "Esempi di domande d'esame su \"{0}\":";
    italian.sample_question_footer =
        "Servono come guida al registro, alla lunghezza e alla difficolta. "
        "Non copiarle parola per parola, non elencarle e non farne piu di una "
        "per volta. La tua domanda deve comunque rispondere a quello che lo "
        "studente ha appena detto.";
    italian.opening_sample_footer =
        "Servono come guida al registro, alla lunghezza e alla difficolta. "
        "Scegline una e falla, quasi con le stesse parole: qui va benissimo. "
        "Non elencarle e non farne piu di una. Non aggiungere dettagli per "
        "renderla piu formale: una domanda corta e gia quella giusta.";

    LanguagePack german;
    german.id = "german";
    german.display_name = "German";
    german.translate_code = "de";
    german.whisper_code = "de";
    german.piper_voice_path = "models/de_DE-thorsten-low.onnx";
    german.prompt_dir = "prompts/german";
    german.opening_turn_text = "Beginne die Pruefung.";
    german.prewarm_text = "Guten Tag.";
    german.opinion_openers = {"deiner meinung nach", "was denkst du",
                              "findest du", "magst du"};
    //ASCII only, and chosen for it: "was haeltst du" is the phrase a German
    //examiner reaches for first and the one asks_opinion() can never match
    german.opinion_opener_label = "Was denkst du";
    //not "Deiner Meinung nach": fronting a dative phrase inverts subject and
    //verb, which is a harder opening than the beginner rules elsewhere allow
    german.student_name_sentence =
        "Der Student heisst {0}. Du weisst bereits, wie er heisst, frage also "
        "niemals nach seinem Namen.";
    german.sample_question_header = "Beispiele fuer Pruefungsfragen zu \"{0}\":";
    german.sample_question_footer =
        "Sie dienen als Richtschnur fuer Register, Laenge und "
        "Schwierigkeitsgrad. Kopiere sie nicht Wort fuer Wort, zaehle sie nicht "
        "auf und stelle nie mehr als eine auf einmal. Deine Frage muss trotzdem "
        "auf das eingehen, was der Student gerade gesagt hat.";
    german.opening_sample_footer =
        "Sie dienen als Richtschnur fuer Register, Laenge und "
        "Schwierigkeitsgrad. Waehle eine aus und stelle sie, fast mit denselben "
        "Worten: das ist hier genau richtig. Zaehle sie nicht auf und stelle "
        "nie mehr als eine. Fuege nichts hinzu, um sie foermlicher zu machen: "
        "eine kurze Frage ist schon die richtige.";

    return {std::move(italian), std::move(german)};
    //ASCII transliterations throughout (heisst, Pruefung, fuer). These strings
    //are compiled in and travel to Gemini as part of a System turn; the prompt
    //files on disk carry the real orthography, and the model reads both alike
}

// LANGUAGE_VOICES wins, then PIPER_MODEL_PATH for italian alone, then the
// voice the pack was built with. Shared by load() and configured_voice_paths()
// so the rate cache and the registry can never name different files.
std::string resolve_voice(const LanguagePack& pack, const Config& config) {
    for (const auto& [id, voice] : config.language_voices) {
        if (id == pack.id && !voice.empty()) {
            return voice;
            //an override rather than a replacement: a .env naming only german
            //leaves italian on its built-in voice
        }
    }
    if (pack.id == kDefaultLanguageId && !config.piper_model_path.empty()) {
        return config.piper_model_path;
        //PIPER_MODEL_PATH still means the Italian voice, so an .env written
        //before this change runs unaltered
    }
    return pack.piper_voice_path;
}

}  // namespace

std::vector<std::string> configured_voice_paths(const Config& config) {
    std::vector<std::string> paths;
    for (const LanguagePack& pack : built_in_packs()) {
        std::string voice = resolve_voice(pack, config);
        if (!voice.empty()) {
            paths.push_back(std::move(voice));
        }
    }
    return paths;
}

LanguageRegistry LanguageRegistry::load(const Config& config) {
    LanguageRegistry registry;
    registry.default_id_ = kDefaultLanguageId;

    for (LanguagePack pack : built_in_packs()) {
        pack.piper_voice_path = resolve_voice(pack, config);

        pack.first_prompt = load_prompt(
            prompt_path(pack, "examiner_first.txt"),
            "You are an examiner. Ask the student one short, easy question in " +
                pack.display_name +
                ", in the present tense, at a beginner's level.");
        pack.ongoing_prompt = load_prompt(
            prompt_path(pack, "examiner_ongoing.txt"),
            "You are an examiner. Ask the student questions in " +
                pack.display_name + " at a beginner's level.");

        pack.question_bank = std::make_shared<const QuestionBank>(
            QuestionBank::load(prompt_path(pack, "question_bank.txt")));

        if (pack.question_bank->empty()) {
            std::cerr << pack.id
                      << ": question bank is missing or carries no questions, "
                         "so the examiner runs without samples"
                      << std::endl;
        } else {
            std::cerr << pack.id << ": " << pack.question_bank->group_count()
                      << " topic groups, voice " << pack.piper_voice_path
                      << std::endl;
        }

        const std::string id = pack.id;
        registry.packs_.emplace(id, std::move(pack));
    }

    return registry;
}

const LanguagePack* LanguageRegistry::find(const std::string& id) const {
    const auto at = packs_.find(id);
    return at == packs_.end() ? nullptr : &at->second;
}

const LanguagePack& LanguageRegistry::default_pack() const {
    const auto at = packs_.find(default_id_);
    if (at != packs_.end()) {
        return at->second;
    }
    return packs_.begin()->second;
    //built_in_packs() always carries italian, so the lookup above is the path
    //taken. The fallback exists so the reference is never dangling if it stops
}

std::vector<const LanguagePack*> LanguageRegistry::all() const {
    std::vector<const LanguagePack*> out;
    out.reserve(packs_.size());
    for (const auto& [id, pack] : packs_) {
        (void)id;
        out.push_back(&pack);
    }
    return out;
}

}  // namespace sim
