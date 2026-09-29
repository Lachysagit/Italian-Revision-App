#pragma once

#include "crow/json.h"

#include "sim/exam_plan.hpp"

namespace sim {

// A plan as JSON: the frozen copy an attempt keeps, and what the plan routes
// return. with_questions false leaves the set questions out, which is the
// student's view - a student picking an exam sees its name and length, never
// the questions they are about to be asked.
crow::json::wvalue plan_to_json(const ExamPlan& plan, bool with_questions);

}  // namespace sim
