#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

// An exam plan: what a teacher asks the examiner to cover. Plain data, in its
// own header because both sides need it - Store reads and writes plans, and
// Session follows one - and Session must not include the Store.

struct PlanQuestion {
    std::int64_t id = 0;
    std::string text;
    //in the exam's language, asked as written unless the plan allows paraphrase
    std::string topic_group;
    //one of the syllabus groups in topics.cpp, or empty for "whenever it fits"
    std::string placement = "any";
    //opening: the first question of the exam. with_topic: asked while its
    //topic is running. any: wherever the exam has room for it
};

struct TenseTarget {
    std::string tense;
    //a canonical key from tenses.hpp - present, perfect, imperfect, future,
    //conditional - the same in every language, so reports compare across them
    int min_count = 1;
};

struct ExamPlan {
    std::int64_t id = 0;
    std::int64_t class_id = 0;
    std::string name;
    int duration_seconds = 0;
    //0 keeps the server's EXAM_DURATION_SECONDS
    bool require_opinion = true;
    bool paraphrase_ok = false;
    bool visible = true;
    //offered to students by name. A hidden plan can still be the class default
    bool archived = false;
    bool is_default = false;
    std::vector<std::string> topics;
    //syllabus groups this exam may cover, in the teacher's order. Empty is all
    std::vector<PlanQuestion> questions;
    std::vector<TenseTarget> tenses;
    std::int64_t updated_at = 0;
};

}  // namespace sim
