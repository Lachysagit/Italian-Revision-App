#pragma once

#include <cstddef>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace sim {

// The syllabus question bank, keyed by the same group strings topic_group()
// returns. Read once at startup and shared read-only by every session, so a
// lookup is an exact match on a key the session already holds.
class QuestionBank {
public:
    // A missing or unreadable file yields an empty bank rather than throwing:
    // an examiner without sample questions is the behaviour of before this
    // file existed, not a broken server. Callers check empty() and log.
    static QuestionBank load(const std::string& path);

    bool empty() const;
    std::size_t group_count() const;

    // A ready-to-send System turn holding up to count questions drawn at
    // random from group, or empty when the group carries none. header and
    // footer are the exam language's own wording, passed in because a bank is
    // loaded from a path and never learns which language it holds; {0} in the
    // header stands for the group.
    std::string examples_for(const std::string& group,
                             std::size_t count,
                             std::mt19937& rng,
                             const std::string& header,
                             const std::string& footer) const;

    // The same, preferring the group's headline questions - the ones marked >
    // in the file, which open a topic rather than follow one up - and topping
    // the count up from its follow-ups when it has too few. Most groups mark
    // exactly one headline, and handing the examiner a single sample is what
    // made it open on the same question every exam.
    std::string openers_for(const std::string& group,
                            std::size_t count,
                            std::mt19937& rng,
                            const std::string& header,
                            const std::string& footer) const;

private:
    struct Question {
        std::string text;
        bool headline = false;
        //the > mark. Kept per question so one pass over the file serves both
        //the opening turn and every later one
    };

    std::string draw(const std::string& group,
                     std::size_t count,
                     std::mt19937& rng,
                     const std::string& header,
                     const std::string& footer,
                     bool headlines_only) const;

    std::map<std::string, std::vector<Question>> by_group_;
};

}  // namespace sim
