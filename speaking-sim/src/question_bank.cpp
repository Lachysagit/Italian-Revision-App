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
        const bool headline = trimmed[0] == '>';
        std::string question = trim(trimmed.substr(1));
        if (!question.empty()) {
            bank.by_group_[group].push_back(
                Question{std::move(question), headline});
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
                                       std::mt19937& rng,
                                       const std::string& header,
                                       const std::string& footer) const {
    return draw(group, count, rng, header, footer, false);
}

std::string QuestionBank::openers_for(const std::string& group,
                                      std::size_t count,
                                      std::mt19937& rng,
                                      const std::string& header,
                                      const std::string& footer) const {
    return draw(group, count, rng, header, footer, true);
}

std::string QuestionBank::draw(const std::string& group,
                               std::size_t count,
                               std::mt19937& rng,
                               const std::string& header,
                               const std::string& footer,
                               bool headlines_only) const {
    const auto entry = by_group_.find(group);
    if (count == 0 || entry == by_group_.end()) {
        return {};
    }

    std::vector<const Question*> picked;
    std::vector<const Question*> rest;
    picked.reserve(entry->second.size());
    for (const Question& question : entry->second) {
        if (!headlines_only || question.headline) {
            picked.push_back(&question);
        } else {
            rest.push_back(&question);
        }
    }
    std::shuffle(picked.begin(), picked.end(), rng);

    if (headlines_only && picked.size() < count) {
        std::shuffle(rest.begin(), rest.end(), rng);
        picked.insert(picked.end(), rest.begin(), rest.end());
        //headlines first, then follow-ups to make up the count. Most groups
        //mark only one headline, and one sample is what the examiner was
        //already doing wrong: it copies the single example every exam. A
        //follow-up asked cold still opens a topic, so the count matters more
        //here than the mark does
    }
    if (picked.size() > count) {
        picked.resize(count);
    }
    if (picked.empty()) {
        return {};
    }

    if (headlines_only) {
        std::shuffle(picked.begin(), picked.end(), rng);
        const auto headline = std::find_if(
            picked.begin(), picked.end(),
            [](const Question* q) { return q->headline; });
        if (headline != picked.end()) {
            std::iter_swap(picked.begin(), headline);
        }
        //the append above left every headline ahead of every follow-up, so the
        //examiner - told to pick one and ask it - took a headline every time
        //and the opening question came from a pool of ten. Shuffled as one
        //list the whole group is in play, and the swap keeps a topic-opening
        //question in front so the exam does not start on a follow-up that
        //assumes an answer nobody has given yet
    }
    //pointers, not copies: the bank outlives the turn being built

    std::string text = header;
    const std::size_t slot = text.find("{0}");
    if (slot != std::string::npos) {
        text.replace(slot, 3, group);
        //{0} is the topic the questions were drawn for. A header without the
        //marker still works, it just does not name the topic
    }
    text += '\n';
    for (const Question* question : picked) {
        text += "- ";
        text += question->text;
        text += '\n';
    }
    text += footer;
    return text;
    //both come from the caller's LanguagePack: they are the exam's own
    //language, and this class is handed a path with no idea which one it read
}

}  // namespace sim
