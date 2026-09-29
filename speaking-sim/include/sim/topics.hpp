#pragma once

#include <array>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace sim {

// The closed set of topic tags. Gemini is given these as a JSON enum and can
// return nothing else; Session maps whatever arrives back onto its group.
constexpr std::array<std::string_view, 20> kTopicTags{
    "family",        "home",           "neighbourhood",
    "people",        "places",         "communities",
    "education",     "university",
    "work",          "part-time job",  "earnings",
    "friends",       "recreation",     "pastimes",
    "holidays",      "travel",         "tourism",
    "future plans",  "dreams",         "aspirations",
};

// The syllabus topic a tag belongs to, or empty for a tag not in the set.
// Counting runs on groups: "family" then "home" is one thread, not two.
std::string topic_group(const std::string& tag);

// The seven syllabus groups as a bullet list, in a random order. Drawn once per
// session, so a fixed list can no longer nudge the examiner towards whichever
// topic happens to sit at the top of it.
struct TopicMenu {
    std::string text;
    std::string first;
    //the group the shuffle put at the top, named so the opening turn can draw
    //its sample questions from that one group and the prompt can order the
    //examiner onto it. A shuffle the prompt was free to ignore was still
    //producing the same opening question every exam
};

TopicMenu topic_menu(std::mt19937& rng,
                     const std::vector<std::string>& allowed = {},
                     const std::string& first = "");
//allowed is an exam plan's topic list: only those groups are offered, still
//shuffled. Empty, or naming nothing known, means every group, as before

std::vector<std::string> all_topic_groups();
//the syllabus groups in kTagGroups order, for the plan editor's checkboxes
bool is_topic_group(const std::string& group);

std::vector<std::string> tags_for_groups(const std::vector<std::string>& groups);
//the tags that map onto those groups, in kTopicTags order: the enum a plan
//narrows the examiner's topic field to. Empty groups means every tag

}  // namespace sim
