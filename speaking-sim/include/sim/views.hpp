#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sim/language.hpp"

namespace sim {

// Turning stored values into the words a page shows.
//
// These lived in teacher.js until the dashboard's tables moved onto the server.
// They are grouped in one place rather than scattered through the route files
// because both class_api.cpp and plan_api.cpp render fragments through them,
// and because a label the teacher reads is worth finding in one search.

std::string format_local_time(std::int64_t seconds);
//"-" for a zero, otherwise the server's own local date and time. teacher.js
//formatted in the browser's zone with toLocaleString; on one machine - which
//is what the README's TZ=Australia/Sydney assumes - those are the same zone.
//A server in another zone to its students would differ, and the swap is here
//rather than hidden in a route so that is easy to find again

std::string format_short_time(std::int64_t seconds);
//the same, without the year: what the student's own exam history showed, where
//every row is recent by nature and the year is the same noise four times over

std::string capitalise(std::string text);
//the first byte, which is the first letter for the values this is used on:
//syllabus group names and the stored subject levels, both ASCII. Never on a
//name somebody typed, which may not start with a single-byte letter at all

std::string end_reason_label(std::int64_t ended_at, const std::string& reason);
//the END_REASONS table teacher.js carried. An exam with no ended_at is still
//running; an unrecognised reason is shown as it was stored rather than hidden,
//so a reason added to the server does not need this edited to appear

std::string tense_label(const LanguagePack& pack, std::string_view key);
//a tense's name in one language's own terms - passato prossimo, Perfekt - from
//the canonical key the database stores. The key itself is the fallback, so a
//pack that names only some of them still reports the rest rather than blanking
//the column

std::string short_length(int seconds);
//"5 min", or "5 min 30 s" when it does not land on a whole minute

std::string format_length(int seconds, int standard);
//an exam plan's length, where zero means "whatever the server's own length is"
//- named rather than blank, because a plan that does not set one is a choice
//and the teacher should be able to see what it works out to

std::string pluralise(int count, const char* one, const char* many);
//"1 student" / "2 students", the join the dashboard does in a dozen places

std::string format_join_code(const std::string& code);
//in two halves, because it is read off a board. Stored and matched without the
//hyphen, so this is presentation only

}  // namespace sim
