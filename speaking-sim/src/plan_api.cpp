#include "sim/server.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sim/http_util.hpp"
#include "sim/plan_json.hpp"
#include "sim/tenses.hpp"
#include "sim/topics.hpp"

namespace sim {

namespace {

std::string tense_label(const LanguagePack& pack, std::string_view key) {
    for (const auto& [id, name] : pack.tense_labels) {
        if (id == key) return name;
    }
    return std::string(key);
    //the canonical key is the fallback, so a pack that names only some of them
    //still reports the rest rather than blanking the column
}

std::string capitalise(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    }
    return text;
    //the syllabus group names are English and ASCII - see topics.cpp - so the
    //first byte is the first letter. Not safe on a name typed by a student,
    //which is why nothing here is passed through it
}

int count_for(const std::map<std::string, int>& counts, const std::string& key) {
    const auto at = counts.find(key);
    return at == counts.end() ? 0 : at->second;
}

}  // namespace

// Exam plans: the teacher's side of what an exam covers, and the student's
// view of which exams a class offers. The same shape as class_api.cpp - check
// who is asking, check the body, one Store call inside guarded() - and the
// same rule that a class which is not the caller's answers 404.

void Server::register_plan_routes() {
    CROW_ROUTE(app_, "/api/classes/<int>/plans").methods("GET"_method, "POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_class_plans(req, class_id);
    });
    //GET: a teacher gets every plan in full, a student the visible ones by
    //name only. POST: a teacher creates one

    CROW_ROUTE(app_, "/api/plans/<int>").methods("GET"_method, "PUT"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t plan_id) {
        return serve_plan(req, plan_id);
    });

    CROW_ROUTE(app_, "/api/plans/<int>/archive").methods("POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t plan_id) {
        return serve_archive_plan(req, plan_id);
    });

    CROW_ROUTE(app_, "/api/classes/<int>/default-plan").methods("POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_default_plan(req, class_id);
    });

    CROW_ROUTE(app_, "/api/exam-options") //HTTP ROUTE ---
    ([this](const crow::request& req) {
        return serve_exam_options(req);
    });
    //what the plan editor offers: the syllabus topics, and the tenses in the
    //class language's own names

    CROW_ROUTE(app_, "/teacher/classes/<int>/coverage") //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_coverage(req, class_id);
    });
    //an HTML fragment, not JSON: the table is the only thing that ever read
    //this and htmx swaps it in whole. Under /teacher rather than /api for the
    //same reason - /api is what the exam page's own JavaScript still speaks
}

crow::response Server::serve_class_plans(const crow::request& req,
                                         std::int64_t class_id) {
    User user;
    if (req.method == crow::HTTPMethod::Get) {
        if (auto refusal = refuse_unless_signed_in(req, user)) {
            return std::move(*refusal);
        }
        return guarded("list plans", [&] {
            const auto klass = store_->class_by_id(class_id);
            const auto role = klass ? store_->class_role(class_id, user.id)
                                    : std::nullopt;
            if (!klass || !role) {
                return json_error(404, "no such class");
            }
            const bool teaches = role == ClassRole::Teacher;

            std::vector<crow::json::wvalue> list;
            for (const ExamPlan& plan : store_->class_plans(class_id, teaches)) {
                if (!teaches && !plan.visible && !plan.is_default) continue;
                list.push_back(plan_to_json(plan, teaches));
            }
            crow::json::wvalue json;
            json["plans"] = std::move(list);
            return json_response(json);
        });
    }

    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }
    if (klass.archived) {
        return json_error(409, "restore the class before adding exams to it");
    }
    if (auto refusal = refuse_if_rate_limited(
            "plan:" + std::to_string(user.id), 60, 60)) {
        return std::move(*refusal);
    }

    std::string error;
    auto plan = plan_from_json(crow::json::load(req.body),
                               config_.exam_duration_seconds, error);
    if (!plan) {
        return json_error(400, error);
    }
    plan->class_id = class_id;

    const bool make_default = [&] {
        const crow::json::rvalue body = crow::json::load(req.body);
        return body && body.has("is_default") &&
               body["is_default"].t() == crow::json::type::True;
    }();

    return guarded("create plan", [&] {
        ExamPlan saved = store_->save_plan(*plan, user.id);
        if (make_default) {
            store_->set_default_plan(class_id, saved.id);
            saved.is_default = true;
        }
        crow::json::wvalue json;
        json["plan"] = plan_to_json(saved, true);
        return json_response(json, 201);
    });
}

crow::response Server::serve_plan(const crow::request& req, std::int64_t plan_id) {
    User user;
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return std::move(*refusal);
    }

    return guarded("plan", [&] {
        const auto existing = store_->plan_by_id(plan_id);
        if (!existing ||
            store_->class_role(existing->class_id, user.id) != ClassRole::Teacher) {
            return json_error(404, "no such exam");
            //a student never reads a plan by id: it would hand them the set
            //questions. The class list gives them what they need to pick one
        }

        if (req.method == crow::HTTPMethod::Get) {
            crow::json::wvalue json;
            json["plan"] = plan_to_json(*existing, true);
            return json_response(json);
        }
        if (auto refusal = refuse_if_rate_limited(
                "plan:" + std::to_string(user.id), 60, 60)) {
            return std::move(*refusal);
        }

        std::string error;
        auto plan = plan_from_json(crow::json::load(req.body),
                                   config_.exam_duration_seconds, error);
        if (!plan) {
            return json_error(400, error);
        }
        plan->id = plan_id;
        plan->class_id = existing->class_id;

        const crow::json::rvalue body = crow::json::load(req.body);
        ExamPlan saved = store_->save_plan(*plan, user.id);
        if (body && body.has("is_default")) {
            const bool wanted = body["is_default"].t() == crow::json::type::True;
            if (wanted && !saved.archived) {
                store_->set_default_plan(saved.class_id, saved.id);
            } else if (!wanted && saved.is_default) {
                store_->set_default_plan(saved.class_id, std::nullopt);
            }
            saved = *store_->plan_by_id(plan_id);
        }
        crow::json::wvalue json;
        json["plan"] = plan_to_json(saved, true);
        return json_response(json);
        //saving never touches an exam already sat: each attempt keeps its own
        //frozen copy, so this changes the next exam and none of the last
    });
}

crow::response Server::serve_archive_plan(const crow::request& req,
                                          std::int64_t plan_id) {
    User user;
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return std::move(*refusal);
    }
    return guarded("archive plan", [&] {
        const auto existing = store_->plan_by_id(plan_id);
        if (!existing ||
            store_->class_role(existing->class_id, user.id) != ClassRole::Teacher) {
            return json_error(404, "no such exam");
        }
        store_->archive_plan(plan_id);
        crow::json::wvalue json;
        json["ok"] = true;
        return json_response(json);
    });
}

crow::response Server::serve_default_plan(const crow::request& req,
                                          std::int64_t class_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    const auto plan_id = int_field(crow::json::load(req.body), "plan_id");
    return guarded("default plan", [&] {
        if (plan_id && *plan_id > 0) {
            const auto plan = store_->plan_by_id(*plan_id);
            if (!plan || plan->class_id != class_id || plan->archived) {
                return json_error(404, "no such exam in this class");
            }
            store_->set_default_plan(class_id, *plan_id);
        } else {
            store_->set_default_plan(class_id, std::nullopt);
            //no id, or 0: the class goes back to exams with no plan at all
        }
        crow::json::wvalue json;
        json["default_plan_id"] = plan_id.value_or(0);
        return json_response(json);
    });
}

crow::response Server::serve_exam_options(const crow::request& req) {
    User user;
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return std::move(*refusal);
    }

    const char* requested = req.url_params.get("language");
    const LanguagePack* pack =
        requested ? languages_.find(requested) : nullptr;
    if (pack == nullptr) {
        pack = &languages_.default_pack();
    }

    crow::json::wvalue json;
    json["language"] = pack->id;
    json["topics"] = all_topic_groups();

    std::vector<crow::json::wvalue> tenses;
    for (const std::string_view key : kTenseKeys) {
        crow::json::wvalue item;
        item["key"] = std::string(key);
        item["label"] = tense_label(*pack, key);
        tenses.push_back(std::move(item));
    }
    json["tenses"] = std::move(tenses);
    json["default_duration_seconds"] = config_.exam_duration_seconds;
    json["min_duration_seconds"] = kMinExamSeconds;
    json["max_duration_seconds"] = kMaxExamSeconds;
    json["seconds_per_question"] = kSecondsPerQuestion;
    //so the editor can say how many set questions fit, and offer only lengths
    //the server would accept, before it saves. The bounds are served rather
    //than written into the page: changing them here must not need the
    //dashboard edited to match
    return json_response(json);
}

crow::response Server::serve_coverage(const crow::request& req,
                                      std::int64_t class_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    return guarded("coverage", [&] {
        struct Student {
            std::map<std::string, int> produced;
            std::map<std::string, int> asked;
            std::map<std::string, int> topics;
        };
        std::map<std::int64_t, Student> by_user;
        for (const CoverageRow& row : store_->class_coverage(class_id)) {
            Student& student = by_user[row.user_id];
            if (row.kind == "tense") {
                (row.role == "student" ? student.produced : student.asked)[row.value] +=
                    row.count;
            } else if (row.kind == "topic") {
                const std::string group = topic_group(row.value);
                student.topics[group.empty() ? row.value : group] += row.count;
                //tags fold into their syllabus group, which is what a teacher
                //plans in: "home" and "family" are one topic on the report
            }
        }

        const LanguagePack* pack = languages_.find(klass.language_id);
        if (pack == nullptr) {
            pack = &languages_.default_pack();
        }
        //the tense names are the class language's own, so an Italian class
        //reads "passato prossimo" where a German one reads "Perfekt"

        std::vector<crow::json::wvalue> columns;
        std::vector<std::pair<std::string, std::string>> ordered;
        for (const std::string_view key : kTenseKeys) {
            std::string label = tense_label(*pack, key);
            crow::json::wvalue column;
            column["label"] = label;
            columns.push_back(std::move(column));
            ordered.emplace_back(std::string(key), std::move(label));
        }
        //one pass, so the header and every row's cells walk kTenseKeys in the
        //same order. The template cannot line them up itself: mustache has no
        //way to look a key up in a map

        std::vector<crow::json::wvalue> students;
        for (const ClassMember& member : store_->class_members(class_id)) {
            if (member.role != ClassRole::Student) continue;
            if (member.attempt_count == 0) continue;
            //a student who has sat nothing is a row of zeroes. The gap this
            //table is read for is a tense missing from exams that happened

            const Student& counts = by_user[member.user_id];
            const std::string who =
                member.display_name.empty() ? member.email : member.display_name;
            const std::string whose =
                member.display_name.empty() ? "This student" : member.display_name;

            std::vector<crow::json::wvalue> cells;
            for (const auto& [key, label] : ordered) {
                const int produced = count_for(counts.produced, key);
                const int asked = count_for(counts.asked, key);
                crow::json::wvalue cell;
                cell["produced"] = produced;
                cell["asked"] = asked;
                cell["cls"] = produced == 0 ? "number gap" : "number";
                //a zero is shaded, because the gap is what a teacher reads
                //this table for: a tense the student has never produced
                cell["title"] = whose + " used the " + label + " in " +
                                std::to_string(produced) +
                                " answers; the examiner asked in it " +
                                std::to_string(asked) + " times";
                cells.push_back(std::move(cell));
            }

            std::string topics;
            for (const auto& [group, count] : counts.topics) {
                if (!topics.empty()) topics += "; ";
                topics += capitalise(group);
            }

            crow::json::wvalue item;
            item["who"] = who;
            item["attempt_count"] = member.attempt_count;
            item["cells"] = std::move(cells);
            item["topics"] = topics.empty() ? "-" : topics;
            students.push_back(std::move(item));
        }

        crow::json::wvalue context;
        context["any"] = !students.empty();
        context["tenses"] = std::move(columns);
        context["students"] = std::move(students);
        return html_fragment(
            crow::mustache::load("coverage.html").render(context).dump());
    });
}

}  // namespace sim
