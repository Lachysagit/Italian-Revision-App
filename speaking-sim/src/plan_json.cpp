#include "sim/plan_json.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "sim/tenses.hpp"
#include "sim/topics.hpp"

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

namespace {

constexpr std::size_t kMaxNameBytes = 80;
constexpr std::size_t kMaxQuestionBytes = 300;
constexpr std::size_t kMaxQuestions = 20;
constexpr int kMaxTenseCount = 5;

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

bool flag(const crow::json::rvalue& body, const char* key, bool fallback) {
    if (!body.has(key)) return fallback;
    if (body[key].t() == crow::json::type::True) return true;
    if (body[key].t() == crow::json::type::False) return false;
    return fallback;
}

}  // namespace

int questions_that_fit(int duration_seconds) {
    return std::max(1, duration_seconds / kSecondsPerQuestion);
}

std::optional<ExamPlan> plan_from_json(const crow::json::rvalue& body,
                                       int default_duration,
                                       std::string& error) {
    if (!body || body.t() != crow::json::type::Object) {
        error = "expected a JSON object";
        return std::nullopt;
    }

    ExamPlan plan;
    if (body.has("name") && body["name"].t() == crow::json::type::String) {
        plan.name = trimmed(std::string(body["name"].s()));
    }
    if (plan.name.empty() || plan.name.size() > kMaxNameBytes) {
        error = "give the exam a name of up to 80 characters";
        return std::nullopt;
    }

    if (body.has("duration_seconds") &&
        body["duration_seconds"].t() == crow::json::type::Number) {
        plan.duration_seconds = static_cast<int>(body["duration_seconds"].i());
    }
    if (plan.duration_seconds < 0) {
        error = "an exam cannot be a negative length";
        return std::nullopt;
        //caught before the bounds check so the sentence names the real
        //mistake: "shorter than 1 minute" reads like a rounding quarrel
    }
    if (plan.duration_seconds != 0 &&
        (plan.duration_seconds < kMinExamSeconds ||
         plan.duration_seconds > kMaxExamSeconds)) {
        error = std::string("an exam runs between ") +
                std::to_string(kMinExamSeconds / 60) + " and " +
                std::to_string(kMaxExamSeconds / 60) + " minutes, or 0 to keep " +
                "the standard length; " +
                std::to_string(plan.duration_seconds) + " seconds is " +
                (plan.duration_seconds < kMinExamSeconds ? "shorter" : "longer") +
                " than that";
        return std::nullopt;
        //refused rather than clamped, unlike EXAM_DURATION_SECONDS: an operator
        //setting an env var is not watching, and a teacher pressing Save is.
        //Silently shortening their exam is how a plan comes to run for a length
        //nobody chose. The bounds are read from the constants so the sentence
        //cannot go stale the way the old fixed "1 and 60 minutes" did
    }

    plan.require_opinion = flag(body, "require_opinion", true);
    plan.paraphrase_ok = flag(body, "paraphrase_ok", false);
    plan.visible = flag(body, "visible", true);

    if (body.has("topics") && body["topics"].t() == crow::json::type::List) {
        for (const auto& item : body["topics"]) {
            if (item.t() != crow::json::type::String) continue;
            const std::string group(item.s());
            if (!is_topic_group(group)) {
                error = "unknown topic: " + group;
                return std::nullopt;
            }
            if (std::find(plan.topics.begin(), plan.topics.end(), group) ==
                plan.topics.end()) {
                plan.topics.push_back(group);
            }
        }
    }

    if (body.has("questions") && body["questions"].t() == crow::json::type::List) {
        int openings = 0;
        for (const auto& item : body["questions"]) {
            if (item.t() != crow::json::type::Object) continue;
            PlanQuestion question;
            if (item.has("text") && item["text"].t() == crow::json::type::String) {
                question.text = trimmed(std::string(item["text"].s()));
            }
            if (question.text.empty()) continue;
            //a blank row in the editor is skipped rather than refused
            if (question.text.size() > kMaxQuestionBytes) {
                error = "keep each set question under 300 characters";
                return std::nullopt;
            }
            if (item.has("topic_group") &&
                item["topic_group"].t() == crow::json::type::String) {
                question.topic_group = std::string(item["topic_group"].s());
            }
            if (!question.topic_group.empty() && !is_topic_group(question.topic_group)) {
                error = "unknown topic on a set question: " + question.topic_group;
                return std::nullopt;
            }
            if (!question.topic_group.empty() && !plan.topics.empty() &&
                std::find(plan.topics.begin(), plan.topics.end(),
                          question.topic_group) == plan.topics.end()) {
                error = "a set question is about \"" + question.topic_group +
                        "\", which this exam does not cover - tick that topic "
                        "or change the question's topic";
                return std::nullopt;
            }
            if (item.has("placement") &&
                item["placement"].t() == crow::json::type::String) {
                question.placement = std::string(item["placement"].s());
            }
            if (question.placement != "opening" && question.placement != "with_topic" &&
                question.placement != "any") {
                error = "placement must be opening, with_topic or any";
                return std::nullopt;
            }
            if (question.placement == "with_topic" && question.topic_group.empty()) {
                error = "a question asked with its topic needs a topic";
                return std::nullopt;
            }
            if (question.placement == "opening" && ++openings > 1) {
                error = "only one set question can open the exam";
                return std::nullopt;
            }
            plan.questions.push_back(std::move(question));
        }
    }
    if (plan.questions.size() > kMaxQuestions) {
        error = "an exam can have at most 20 set questions";
        return std::nullopt;
    }

    if (body.has("tenses") && body["tenses"].t() == crow::json::type::List) {
        std::set<std::string> seen;
        for (const auto& item : body["tenses"]) {
            if (item.t() != crow::json::type::Object || !item.has("tense") ||
                item["tense"].t() != crow::json::type::String) {
                continue;
            }
            TenseTarget target;
            target.tense = std::string(item["tense"].s());
            if (!is_tense_key(target.tense)) {
                error = "unknown tense: " + target.tense;
                return std::nullopt;
            }
            if (item.has("min_count") && item["min_count"].t() == crow::json::type::Number) {
                target.min_count = static_cast<int>(item["min_count"].i());
            }
            target.min_count = std::clamp(target.min_count, 1, kMaxTenseCount);
            if (seen.insert(target.tense).second) plan.tenses.push_back(target);
        }
    }

    const int duration = plan.duration_seconds > 0 ? plan.duration_seconds
                                                   : default_duration;
    const int room = questions_that_fit(duration) - 1;
    //one question left over for the examiner's own follow-ups at the least
    if (static_cast<int>(plan.questions.size()) > room) {
        const int fits = std::max(0, room);
        error = "a " + std::to_string(duration / 60) + "-minute exam has room for " +
                std::to_string(fits) + (fits == 1 ? " set question" : " set questions") +
                "; remove some or make the exam longer";
        return std::nullopt;
        //refused at save rather than discovered in the exam: the scheduler
        //would otherwise give up on the ones that never fitted and mark them
        //missed against a student who had no chance to answer them
    }
    return plan;
}

}  // namespace sim
