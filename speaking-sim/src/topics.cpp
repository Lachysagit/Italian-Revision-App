#include "sim/topics.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <vector>

namespace sim {

namespace {

struct TagGroup {
    std::string_view tag;
    std::string_view group;
};

// Every tag in kTopicTags appears here exactly once, so an enum-constrained
// reply can never map to nothing.
constexpr std::array<TagGroup, kTopicTags.size()> kTagGroups{{
    {"family", "family life, home and neighbourhood"},
    {"home", "family life, home and neighbourhood"},
    {"neighbourhood", "family life, home and neighbourhood"},
    {"people", "people, places and communities"},
    {"places", "people, places and communities"},
    {"communities", "people, places and communities"},
    {"education", "education and study"},
    {"university", "education and study"},
    {"work", "part-time work and earnings"},
    {"part-time job", "part-time work and earnings"},
    {"earnings", "part-time work and earnings"},
    //a casual job is its own thread, so "work" leaves the education group with
    //it: left behind, it would let a job question count as education
    {"friends", "friends, recreation and pastimes"},
    {"recreation", "friends, recreation and pastimes"},
    {"pastimes", "friends, recreation and pastimes"},
    {"holidays", "holidays, travel and tourism"},
    {"travel", "holidays, travel and tourism"},
    {"tourism", "holidays, travel and tourism"},
    {"future plans", "future plans and aspirations"},
    {"dreams", "future plans and aspirations"},
    {"aspirations", "future plans and aspirations"},
}};

}  // namespace

std::string topic_group(const std::string& tag) {
    for (const TagGroup& entry : kTagGroups) {
        if (entry.tag == tag) {
            return std::string(entry.group);
        }
    }
    return {};
    //eighteen entries, once per turn: a linear scan is the honest shape here
}

std::string topic_menu(std::mt19937& rng) {
    std::vector<std::string_view> groups;
    for (const TagGroup& entry : kTagGroups) {
        if (std::find(groups.begin(), groups.end(), entry.group) ==
            groups.end()) {
            groups.push_back(entry.group);
        }
    }
    //read off kTagGroups rather than listed again here, so the menu and the
    //groups the exam actually counts on cannot drift apart

    std::shuffle(groups.begin(), groups.end(), rng);

    std::string menu;
    for (const std::string_view group : groups) {
        if (!menu.empty()) {
            menu += '\n';
        }
        menu += "- ";
        menu += static_cast<char>(std::toupper(
            static_cast<unsigned char>(group.front())));
        menu.append(group.substr(1));
    }
    return menu;
    //capitalised to match the wording the prompt files carried before the list
    //moved out of them. No trailing newline: the placeholder sits on its own
    //line, so the file supplies the break after it
}

}  // namespace sim
