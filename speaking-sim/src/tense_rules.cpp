#include "sim/tenses.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace sim {

namespace {

// ---- splitting ------------------------------------------------------------

// Lower case, ASCII letters only: every accented letter in these languages is
// already lower case in the forms that matter (andrò, città), and UTF-8 bytes
// pass through untouched as parts of a word.
std::string lowered(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool is_word_byte(unsigned char ch) {
    return std::isalpha(ch) != 0 || ch >= 0x80;
    //bytes of a multi-byte UTF-8 letter are all >= 0x80, so they stay inside
    //the word. An apostrophe splits, which turns "l'ho" into "l" and "ho"
}

// Sentences of words. The German rules look at the end of a clause, where the
// participle or the infinitive goes, so the split has to keep sentences apart.
std::vector<std::vector<std::string>> sentences_of(const std::string& text) {
    std::vector<std::vector<std::string>> sentences(1);
    std::string word;
    const auto flush_word = [&] {
        if (!word.empty()) {
            sentences.back().push_back(word);
            word.clear();
        }
    };

    for (const char raw : lowered(text)) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (is_word_byte(ch)) {
            word.push_back(raw);
            continue;
        }
        flush_word();
        if (raw == '.' || raw == '?' || raw == '!' || raw == ';' || raw == ',') {
            if (!sentences.back().empty()) sentences.emplace_back();
            //a comma ends a clause too, which is where the German verb sits:
            //"Ich habe gegessen, weil ..." puts the participle before it
        }
    }
    flush_word();
    if (sentences.back().empty()) sentences.pop_back();
    return sentences;
}

bool ends_with(const std::string& word, const std::string& tail) {
    return word.size() >= tail.size() &&
           word.compare(word.size() - tail.size(), tail.size(), tail) == 0;
}

bool starts_with(const std::string& word, const std::string& head) {
    return word.compare(0, head.size(), head) == 0;
}

bool ends_with_any(const std::string& word, const std::vector<std::string>& tails,
                   std::size_t min_stem) {
    for (const std::string& tail : tails) {
        if (ends_with(word, tail) && word.size() >= tail.size() + min_stem) {
            return true;
        }
    }
    return false;
}

// ---- Italian ----------------------------------------------------------------

const std::set<std::string> kItalianAux{
    "ho", "hai", "ha", "abbiamo", "avete", "hanno",
    "sono", "sei", "\xc3\xa8", "siamo", "siete",
};
//"è" spelled as its UTF-8 bytes so the file's own encoding cannot change it

const std::set<std::string> kItalianBetween{
    "gi\xc3\xa0", "mai", "sempre", "anche", "appena", "ancora", "non", "ci",
    "ne", "lo", "la", "li", "le", "mi", "ti", "si", "vi", "gli", "tanto",
    "molto", "poco", "bene", "male", "davvero",
};
//the short words that can sit between an auxiliary and its participle: "ho
//già mangiato", "sono appena arrivata". Anything else in between ("ha una
//vita") means the auxiliary was a plain verb, not a perfect

const std::set<std::string> kItalianIrregularParticiples{
    "fatto", "fatta", "fatti", "fatte", "detto", "detta", "visto", "vista",
    "visti", "viste", "preso", "presa", "presi", "prese", "messo", "messa",
    "letto", "letta", "scritto", "scritta", "stato", "stata", "stati", "state",
    "rimasto", "rimasta", "rimasti", "nato", "nata", "nati", "morto", "morta",
    "aperto", "aperta", "chiuso", "chiusa", "vissuto", "corso", "perso", "persa",
    "scelto", "scelta", "risposto", "chiesto", "speso", "successo", "offerto",
    "vinto", "rotto", "sceso", "scesa", "acceso", "deciso", "venuto", "venuta",
    "venuti", "piaciuto", "piaciuta", "piaciuti", "bevuto", "conosciuto",
};

bool italian_participle(const std::string& word) {
    if (kItalianIrregularParticiples.count(word)) return true;
    return ends_with_any(word, {"ato", "ata", "ati", "ate", "uto", "uta", "uti",
                                "ute", "ito", "ita", "iti", "ite"}, 2);
}

const std::set<std::string> kItalianNotConditional{"foreste", "celeste"};
const std::set<std::string> kItalianNotFuture{
    "per\xc3\xb2", "remo", "estremo", "supremo", "rete", "parete", "segrete",
    "sirene", "carai", "orai",
};
const std::set<std::string> kItalianNotImperfect{
    "bravo", "brava", "bravi", "brave", "cattivo", "cattiva", "cattivi",
    "attivo", "attiva", "attivi", "vivo", "viva", "vivi", "arrivo", "arriva",
    "arrivi", "scrivo", "scrivi", "motivo", "motivi", "obiettivo", "sportivo",
    "sportiva", "sportivi", "positivo", "positiva", "negativo", "negativa",
    "creativo", "creativa", "nuovo", "nuova", "nuovi", "ottavo", "ottava",
    "chiavi", "lavoro", "lava", "cava", "riva", "diva", "iva", "gravi",
    "archivio", "divi", "privo", "priva", "festivi", "cibi", "evo", "lievi",
    "nevi", "eva", "decisivo", "definitivo", "estivo", "estiva", "estivi",
    "cattive", "rivi", "tardivo", "vivono",
};
//real words whose endings look like an imperfect. The one that matters most is
//the -ivo family, which is the present of verbs like vivere and arrivare

std::vector<std::string> italian_tenses(const std::string& text) {
    std::set<std::string> found;
    for (const std::vector<std::string>& sentence : sentences_of(text)) {
        for (std::size_t i = 0; i < sentence.size(); ++i) {
            const std::string& word = sentence[i];

            if (!kItalianNotConditional.count(word) &&
                ends_with_any(word, {"rei", "resti", "rebbe", "remmo", "reste",
                                     "rebbero"}, 2)) {
                found.insert("conditional");
            }

            if (!kItalianNotFuture.count(word) &&
                ends_with_any(word, {"r\xc3\xb2", "rai", "r\xc3\xa0", "remo",
                                     "rete", "ranno"}, 2)) {
                found.insert("future");
            }
            //ò and à as bytes again: andrò, farà

            if (word == "ero" || word == "eri" || word == "era" ||
                word == "eravamo" || word == "eravate" || word == "erano" ||
                (!kItalianNotImperfect.count(word) &&
                 ends_with_any(word, {"avo", "avi", "ava", "avamo", "avate",
                                      "avano", "evo", "evi", "eva", "evamo",
                                      "evate", "evano", "ivo", "ivi", "iva",
                                      "ivamo", "ivate", "ivano"}, 2))) {
                found.insert("imperfect");
            }

            if (kItalianAux.count(word)) {
                for (std::size_t j = i + 1; j < sentence.size() && j <= i + 3; ++j) {
                    if (italian_participle(sentence[j])) {
                        found.insert("perfect");
                        break;
                    }
                    if (!kItalianBetween.count(sentence[j])) break;
                }
            }
        }
    }
    return {found.begin(), found.end()};
}

// ---- German -----------------------------------------------------------------

// Each word in both spellings: the examiner writes ä, a transcript sometimes
// comes back with ae, and the rules should not care which.
const std::set<std::string> kGermanConditional{
    "w\xc3\xbcrde", "w\xc3\xbcrdest", "w\xc3\xbcrden", "w\xc3\xbcrdet",
    "wuerde", "wuerdest", "wuerden", "wuerdet",
    "h\xc3\xa4tte", "h\xc3\xa4ttest", "h\xc3\xa4tten", "h\xc3\xa4ttet",
    "haette", "haettest", "haetten", "haettet",
    "w\xc3\xa4re", "w\xc3\xa4rst", "w\xc3\xa4ren", "w\xc3\xa4rt",
    "waere", "waerst", "waeren", "waert",
    "k\xc3\xb6nnte", "k\xc3\xb6nntest", "k\xc3\xb6nnten", "k\xc3\xb6nntet",
    "koennte", "koenntest", "koennten", "koenntet",
};
//"möchte" is left out on purpose: it is Konjunktiv II in form, but a beginner
//says "ich möchte" as a fixed phrase for "I want", and counting it would make
//every exam look as if the conditional had been practised

const std::set<std::string> kGermanFutureAux{
    "werde", "wirst", "wird", "werden", "werdet",
};

const std::set<std::string> kGermanPerfectAux{
    "habe", "hast", "hat", "haben", "habt", "bin", "bist", "ist", "sind", "seid",
};

const std::set<std::string> kGermanPreterite{
    "war", "warst", "waren", "wart", "hatte", "hattest", "hatten", "hattet",
    "ging", "gingen", "kam", "kamen", "sah", "sahen", "fuhr", "fuhren", "gab",
    "machte", "machten", "spielte", "spielten", "wohnte", "wohnten", "wollte",
    "wollten", "konnte", "konnten", "musste", "mussten", "durfte", "durften",
    "lebte", "lebten", "arbeitete", "arbeiteten", "sagte", "sagten", "fand",
    "fanden", "blieb", "blieben", "a\xc3\x9f", "trank", "tranken",
    "schlief", "lief", "liefen", "wusste", "wussten",
};
//a word list, not the -te ending: "heute", "Leute" and "gute" all end in -te
//and none of them is a verb

const std::vector<std::string> kGermanSeparablePrefixes{
    "ab", "an", "auf", "aus", "ein", "mit", "nach", "vor", "zu", "zur\xc3\xbc" "ck",
    "fern", "los", "weg", "her", "hin", "fest", "kennen",
};

const std::set<std::string> kGermanInseparableParticiples{
    "besucht", "bekommen", "verloren", "vergessen", "verstanden", "erz\xc3\xa4hlt",
    "erlebt", "besichtigt", "\xc3\xbc" "bernachtet", "entschieden", "verbracht",
    "begonnen", "bestellt", "bezahlt", "verdient", "verkauft", "gewesen",
    "geworden", "erreicht", "versucht", "erkl\xc3\xa4rt", "vermisst",
};

bool german_participle(const std::string& word) {
    if (kGermanInseparableParticiples.count(word)) return true;
    if (!(ends_with(word, "t") || ends_with(word, "en"))) return false;
    if (ends_with(word, "iert") && word.size() > 5) return true;
    if (starts_with(word, "ge") && word.size() > 4) return true;
    for (const std::string& prefix : kGermanSeparablePrefixes) {
        if (starts_with(word, prefix) &&
            word.compare(prefix.size(), 2, "ge") == 0 &&
            word.size() > prefix.size() + 4) {
            return true;
            //eingekauft, aufgestanden, ferngesehen: the ge- sits after the
            //separable prefix rather than at the front
        }
    }
    return false;
}

bool german_infinitive(const std::string& word) {
    return word == "sein" || word == "tun" ||
           (word.size() > 3 && (ends_with(word, "en") || ends_with(word, "ern") ||
                                ends_with(word, "eln")));
}

std::vector<std::string> german_tenses(const std::string& text) {
    std::set<std::string> found;
    for (const std::vector<std::string>& clause : sentences_of(text)) {
        bool perfect_aux = false;
        bool future_aux = false;
        for (std::size_t i = 0; i < clause.size(); ++i) {
            const std::string& word = clause[i];
            if (kGermanConditional.count(word)) found.insert("conditional");
            if (kGermanPreterite.count(word)) found.insert("imperfect");
            if (kGermanPerfectAux.count(word)) perfect_aux = true;
            if (kGermanFutureAux.count(word)) future_aux = true;
        }
        if (clause.size() < 2) continue;

        const std::string& last = clause.back();
        //German puts the participle and the infinitive at the end of the
        //clause, which is the whole of what these two checks lean on
        if (perfect_aux && !kGermanPerfectAux.count(last) && german_participle(last)) {
            found.insert("perfect");
        }
        if (future_aux && !kGermanFutureAux.count(last) && german_infinitive(last)) {
            found.insert("future");
            //"Es wird kalt" has no infinitive at the end and is not a future;
            //"Ich werde nach Berlin fahren" does
        }
    }
    return {found.begin(), found.end()};
}

}  // namespace

bool is_tense_key(const std::string& key) {
    return std::find(kTenseKeys.begin(), kTenseKeys.end(), key) != kTenseKeys.end();
}

std::vector<std::string> detect_tenses(const std::string& language_id,
                                       const std::string& text) {
    if (language_id == "italian") return italian_tenses(text);
    if (language_id == "german") return german_tenses(text);
    return {};
}

int check_tense_rules() {
    struct Case {
        const char* language;
        const char* text;
        std::vector<std::string> expected;
    };
    const std::vector<Case> cases{
        {"italian", "Abito a Sydney con la mia famiglia.", {}},
        {"italian", "Sabato sono andata al mare con mia sorella.", {"perfect"}},
        {"italian", "Ho gi\xc3\xa0 mangiato la pizza.", {"perfect"}},
        {"italian", "L'ho fatto ieri.", {"perfect"}},
        {"italian", "Mia nonna ha una vita tranquilla.", {}},
        {"italian", "Quando ero piccolo giocavo a calcio.", {"imperfect"}},
        {"italian", "Vivo in un appartamento e arrivo sempre in ritardo.", {}},
        {"italian", "L'anno prossimo andr\xc3\xb2 in Italia e visiter\xc3\xb2 Roma.", {"future"}},
        {"italian", "Per\xc3\xb2 non lo so.", {}},
        {"italian", "Mi piacerebbe fare il medico, vorrei aiutare la gente.", {"conditional"}},
        {"italian", "Cosa faresti se vincessi la lotteria?", {"conditional"}},
        {"italian", "Ieri sono uscita, domani uscir\xc3\xb2 di nuovo.", {"future", "perfect"}},
        {"german", "Ich wohne in Sydney.", {}},
        {"german", "Am Wochenende habe ich Fu\xc3\x9f" "ball gespielt.", {"perfect"}},
        {"german", "Ich bin nach Berlin gefahren.", {"perfect"}},
        {"german", "Gestern habe ich eingekauft.", {"perfect"}},
        {"german", "Ich habe einen Hund.", {}},
        {"german", "Das ist gut.", {}},
        {"german", "Letztes Jahr war ich in Deutschland.", {"imperfect"}},
        {"german", "Heute kommen viele Leute.", {}},
        {"german", "N\xc3\xa4" "chstes Jahr werde ich nach \xc3\x96sterreich fahren.", {"future"}},
        {"german", "Es wird kalt.", {}},
        {"german", "Ich w\xc3\xbcrde gern Arzt werden.", {"conditional"}},
        {"german", "Wenn ich reich w\xc3\xa4re, h\xc3\xa4tte ich ein Haus.", {"conditional"}},
        {"german", "Ich m\xc3\xb6" "chte ein Eis.", {}},
        {"french", "J'ai mang\xc3\xa9.", {}},
    };

    int failures = 0;
    for (const Case& c : cases) {
        std::vector<std::string> got = detect_tenses(c.language, c.text);
        std::vector<std::string> want = c.expected;
        std::sort(want.begin(), want.end());
        const bool ok = got == want;
        failures += ok ? 0 : 1;

        std::cout << (ok ? "  ok   " : "  FAIL ") << c.language << ": " << c.text
                  << " -> [";
        for (std::size_t i = 0; i < got.size(); ++i) {
            std::cout << (i ? ", " : "") << got[i];
        }
        std::cout << "]";
        if (!ok) {
            std::cout << " expected [";
            for (std::size_t i = 0; i < want.size(); ++i) {
                std::cout << (i ? ", " : "") << want[i];
            }
            std::cout << "]";
        }
        std::cout << "\n";
    }
    std::cout << (failures == 0 ? "tense rules: ok\n" : "tense rules: FAILED\n");
    return failures == 0 ? 0 : 1;
}

}  // namespace sim
