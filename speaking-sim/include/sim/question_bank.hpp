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
    // random from group, or empty when the group carries none.
    std::string examples_for(const std::string& group,
                             std::size_t count,
                             std::mt19937& rng) const;

private:
    std::map<std::string, std::vector<std::string>> by_group_;
};

}  // namespace sim
