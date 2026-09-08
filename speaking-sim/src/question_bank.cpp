#include "sim/question_bank.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

namespace sim {

namespace {

// \r included: the file is edited on Windows, so every line arrives with one.
std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

}  // namespace

QuestionBank QuestionBank::load(const std::string& path) {
    QuestionBank bank;
    std::ifstream file(path);
    if (!file) {
        return bank;
    }

    std::string line;
    std::string group;
    while (std::getline(file, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.compare(0, 2, "##") == 0) {
            group = trim(trimmed.substr(2));
            continue;
            //checked before the comment mark, because ## starts with #
        }
        if (group.empty() || (trimmed[0] != '>' && trimmed[0] != '-')) {
            continue;
            //a comment, or a question sitting above the first group header
        }
        std::string question = trim(trimmed.substr(1));
        if (!question.empty()) {
            bank.by_group_[group].push_back(std::move(question));
        }
    }
    return bank;
    //a group header with no questions under it never reaches the map, so
    //group_count() counts groups that can actually be drawn from
}

bool QuestionBank::empty() const {
    return by_group_.empty();
}

std::size_t QuestionBank::group_count() const {
    return by_group_.size();
}

std::string QuestionBank::examples_for(const std::string& group,
                                       std::size_t count,
                                       std::mt19937& rng) const {
    const auto entry = by_group_.find(group);
    if (count == 0 || entry == by_group_.end()) {
        return {};
    }

    std::vector<const std::string*> picked;
    picked.reserve(entry->second.size());
    for (const std::string& question : entry->second) {
        picked.push_back(&question);
    }
    std::shuffle(picked.begin(), picked.end(), rng);
    if (picked.size() > count) {
        picked.resize(count);
    }
    //pointers, not copies: the bank outlives the turn being built

    std::string text = "Esempi di domande d'esame su \"" + group + "\":\n";
    for (const std::string* question : picked) {
        text += "- ";
        text += *question;
        text += '\n';
    }
    text +=
        "Servono come guida al registro, alla lunghezza e alla difficolta. "
        "Non copiarle parola per parola, non elencarle e non farne piu di una "
        "per volta. La tua domanda deve comunque rispondere a quello che lo "
        "studente ha appena detto.";
    return text;
}

}  // namespace sim
