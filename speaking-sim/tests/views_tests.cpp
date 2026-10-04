// Offline tests for the presentation helpers the server-rendered fragments go
// through. Same shape as safety_tests.cpp: asserts rather than a framework,
// and nothing here touches the network, the database or Crow.
//
//   cmake --build build --target views-tests && ./build/views-tests
//
// The time format is the reason this file exists. These labels used to be
// built in teacher.js by toLocaleString("en-AU", {dateStyle, timeStyle}), and
// matching it in strftime means tidying %e, %l and %p by hand - which is easy
// to get subtly wrong in a way only a May or an August would show.

#include "sim/views.hpp"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void equal(const std::string& got, const std::string& want,
           const std::string& what) {
    if (got == want) return;
    std::cerr << "FAIL: " << what << " (got \"" << got << "\", want \""
              << want << "\")\n";
    ++failures;
}

// A fixed local instant, built through mktime so the test does not depend on
// the zone it runs in.
std::int64_t local_instant(int year, int month, int day, int hour, int minute) {
    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&parts));
}

void test_time_format() {
    using sim::format_local_time;

    equal(format_local_time(0), "-", "a zero is a dash, not the epoch");
    equal(format_local_time(local_instant(2026, 10, 4, 13, 23)),
          "4 Oct 2026, 1:23 pm", "afternoon, single-digit day and hour");
    equal(format_local_time(local_instant(2026, 10, 4, 9, 5)),
          "4 Oct 2026, 9:05 am", "morning, and a minute that needs its zero");
    equal(format_local_time(local_instant(2026, 11, 20, 0, 0)),
          "20 Nov 2026, 12:00 am", "midnight is 12 am, not 0 am");
    equal(format_local_time(local_instant(2026, 11, 20, 12, 0)),
          "20 Nov 2026, 12:00 pm", "noon is 12 pm");

    // The months that share a letter with AM/PM: a sweep for A, P or M rather
    // than the final two characters lower-cases these as well.
    equal(format_local_time(local_instant(2026, 5, 1, 14, 0)),
          "1 May 2026, 2:00 pm", "May keeps its capital M");
    equal(format_local_time(local_instant(2026, 4, 1, 14, 0)),
          "1 Apr 2026, 2:00 pm", "Apr keeps its capital A");
    equal(format_local_time(local_instant(2026, 8, 1, 14, 0)),
          "1 Aug 2026, 2:00 pm", "Aug keeps its capital A");
    equal(format_local_time(local_instant(2026, 3, 1, 14, 0)),
          "1 Mar 2026, 2:00 pm", "Mar keeps its capital M");
    equal(format_local_time(local_instant(2026, 9, 1, 14, 0)),
          "1 Sep 2026, 2:00 pm", "Sep keeps its capital P");
}

void test_short_time_format() {
    using sim::format_short_time;

    equal(format_short_time(0), "-", "a zero is a dash here too");
    equal(format_short_time(local_instant(2026, 10, 4, 13, 23)),
          "4 Oct, 1:23 pm", "no year: the history table is recent by nature");
    equal(format_short_time(local_instant(2026, 5, 4, 13, 23)),
          "4 May, 1:23 pm", "May keeps its capital in the short form too");
    equal(format_short_time(local_instant(2026, 11, 20, 0, 0)),
          "20 Nov, 12:00 am", "midnight, two-digit day");
}

void test_capitalise() {
    equal(sim::capitalise("school"), "School", "a group name");
    equal(sim::capitalise(""), "", "an empty string stays empty");
    equal(sim::capitalise("School"), "School", "already capital");
    equal(sim::capitalise("beginner"), "Beginner", "a subject level");
}

void test_end_reason() {
    using sim::end_reason_label;
    equal(end_reason_label(0, "timer"), "In progress",
          "no ended_at wins over whatever reason was stored");
    equal(end_reason_label(1, "student_end"), "Ended by student", "known reason");
    equal(end_reason_label(1, "quota"), "Out of questions for today",
          "the quota ending teacher.js never listed");
    equal(end_reason_label(1, "something_new"), "something_new",
          "an unknown reason shows as stored rather than vanishing");
    equal(end_reason_label(1, ""), "Ended", "ended with no reason recorded");
}

void test_join_code() {
    equal(sim::format_join_code("ABCDEFGH"), "ABCD-EFGH", "eight splits in two");
    equal(sim::format_join_code(""), "", "joining switched off");
    equal(sim::format_join_code("SHORT"), "SHORT",
          "anything but eight is left alone rather than cut");
}

}  // namespace

int main() {
    test_time_format();
    test_short_time_format();
    test_capitalise();
    test_end_reason();
    test_join_code();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all view checks passed\n";
    return 0;
}
