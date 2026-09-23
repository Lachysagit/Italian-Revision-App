#pragma once

#include <array>
#include <random>
#include <string>
#include <string_view>

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

TopicMenu topic_menu(std::mt19937& rng);

}  // namespace sim
