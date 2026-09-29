#pragma once

#include <optional>
#include <string>

#include "crow/json.h"

#include "sim/exam_plan.hpp"

namespace sim {

// A plan as JSON: the frozen copy an attempt keeps, and what the plan routes
// return. with_questions false leaves the set questions out, which is the
// student's view - a student picking an exam sees its name and length, never
// the questions they are about to be asked.
crow::json::wvalue plan_to_json(const ExamPlan& plan, bool with_questions);

// A plan from the editor's JSON, checked field by field. nullopt with error
// set to a sentence the teacher can act on. default_duration is the server's
// exam length, used when the plan keeps it, for the does-it-fit check.
std::optional<ExamPlan> plan_from_json(const crow::json::rvalue& body,
                                       int default_duration,
                                       std::string& error);

// A question and a beginner's answer to it, the pace a plan is checked
// against when it is saved.
constexpr int kSecondsPerQuestion = 30;

// How many questions an exam of this length has room for at that pace.
int questions_that_fit(int duration_seconds);

}  // namespace sim
