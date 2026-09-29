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

std::vector<std::string> all_topic_groups() {
    std::vector<std::string> groups;
    for (const TagGroup& entry : kTagGroups) {
        const std::string group(entry.group);
        if (std::find(groups.begin(), groups.end(), group) == groups.end()) {
            groups.push_back(group);
        }
    }
    return groups;
    //read off kTagGroups rather than listed again here, so the menu and the
    //groups the exam actually counts on cannot drift apart
}

bool is_topic_group(const std::string& group) {
    const std::vector<std::string> groups = all_topic_groups();
    return std::find(groups.begin(), groups.end(), group) != groups.end();
}

std::vector<std::string> tags_for_groups(const std::vector<std::string>& groups) {
    std::vector<std::string> tags;
    for (const TagGroup& entry : kTagGroups) {
        if (groups.empty() ||
            std::find(groups.begin(), groups.end(), entry.group) != groups.end()) {
            tags.emplace_back(entry.tag);
        }
    }
    return tags;
}

TopicMenu topic_menu(std::mt19937& rng, const std::vector<std::string>& allowed,
                     const std::string& first) {
    std::vector<std::string_view> groups;
    for (const TagGroup& entry : kTagGroups) {
        if (std::find(groups.begin(), groups.end(), entry.group) !=
            groups.end()) {
            continue;
        }
        if (!allowed.empty() &&
            std::find(allowed.begin(), allowed.end(), entry.group) == allowed.end()) {
            continue;
            //a plan that names its topics gets only those in the menu, so the
            //examiner is never offered a topic the teacher left out
        }
        groups.push_back(entry.group);
    }
    if (groups.empty()) {
        return topic_menu(rng, {}, first);
        //a plan naming only unknown groups falls back to the whole syllabus
        //rather than an exam with nothing to talk about
    }

    std::shuffle(groups.begin(), groups.end(), rng);
    const auto pinned = std::find(groups.begin(), groups.end(), first);
    if (!first.empty() && pinned != groups.end()) {
        std::rotate(groups.begin(), pinned, pinned + 1);
        //a plan's set opening question belongs to one topic, so that topic
        //opens the exam and the rest keep their shuffled order behind it
    }

    TopicMenu menu;
    if (!groups.empty()) {
        menu.first = std::string(groups.front());
        //the group string as the bank keys it, uncapitalised: examples_for()
        //and openers_for() match it exactly
    }
    for (const std::string_view group : groups) {
        if (!menu.text.empty()) {
            menu.text += '\n';
        }
        menu.text += "- ";
        menu.text += static_cast<char>(std::toupper(
            static_cast<unsigned char>(group.front())));
        menu.text.append(group.substr(1));
    }
    return menu;
    //capitalised to match the wording the prompt files carried before the list
    //moved out of them. No trailing newline: the placeholder sits on its own
    //line, so the file supplies the break after it
}

}  // namespace sim
