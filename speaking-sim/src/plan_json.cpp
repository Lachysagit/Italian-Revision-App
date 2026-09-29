#include "sim/plan_json.hpp"

#include <utility>
#include <vector>

namespace sim {

crow::json::wvalue plan_to_json(const ExamPlan& plan, bool with_questions) {
    crow::json::wvalue json;
    json["id"] = plan.id;
    json["class_id"] = plan.class_id;
    json["name"] = plan.name;
    json["duration_seconds"] = plan.duration_seconds;
    json["is_default"] = plan.is_default;
    json["visible"] = plan.visible;
    json["archived"] = plan.archived;
    json["updated_at"] = plan.updated_at;
    if (!with_questions) {
        return json;
    }

    json["require_opinion"] = plan.require_opinion;
    json["paraphrase_ok"] = plan.paraphrase_ok;
    json["topics"] = plan.topics;

    std::vector<crow::json::wvalue> questions;
    for (const PlanQuestion& question : plan.questions) {
        crow::json::wvalue item;
        item["id"] = question.id;
        item["text"] = question.text;
        item["topic_group"] = question.topic_group;
        item["placement"] = question.placement;
        questions.push_back(std::move(item));
    }
    json["questions"] = std::move(questions);

    std::vector<crow::json::wvalue> tenses;
    for (const TenseTarget& target : plan.tenses) {
        crow::json::wvalue item;
        item["tense"] = target.tense;
        item["min_count"] = target.min_count;
        tenses.push_back(std::move(item));
    }
    json["tenses"] = std::move(tenses);
    return json;
}

}  // namespace sim
