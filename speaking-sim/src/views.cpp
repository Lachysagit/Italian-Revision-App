#include "sim/views.hpp"

#include <array>
#include <cctype>
#include <ctime>
#include <string>
#include <utility>

namespace sim {

namespace {

// strftime plus the tidying that makes it read like
// toLocaleString("en-AU", ...) did in the browser: %e and %l pad a single digit
// with a space, and %p is upper case.
std::string format_with(std::int64_t seconds, const char* pattern) {
    if (seconds == 0) {
        return "-";
    }
    const std::time_t stamp = static_cast<std::time_t>(seconds);
    std::tm parts{};
#if defined(_WIN32)
    if (localtime_s(&parts, &stamp) != 0) return "-";
#else
    if (localtime_r(&stamp, &parts) == nullptr) return "-";
#endif
    char buffer[64];
    if (std::strftime(buffer, sizeof(buffer), pattern, &parts) == 0) {
        return "-";
    }

    std::string text(buffer);
    std::size_t at = 0;
    while ((at = text.find("  ", at)) != std::string::npos) {
        text.erase(at, 1);
    }
    while (!text.empty() && text.front() == ' ') {
        text.erase(0, 1);
    }

    if (text.size() >= 2) {
        char& meridiem = text[text.size() - 2];
        char& mark = text[text.size() - 1];
        if ((meridiem == 'A' || meridiem == 'P') && mark == 'M') {
            meridiem = static_cast<char>(std::tolower(static_cast<unsigned char>(meridiem)));
            mark = 'm';
        }
    }
    //only the final two characters, never a sweep for A, P or M: "4 Apr",
    //"4 Aug", "4 May", "4 Mar" and "4 Sep" all carry one and keep their capital
    return text;
}

}  // namespace

std::string format_local_time(std::int64_t seconds) {
    return format_with(seconds, "%e %b %Y, %l:%M %p");
}

std::string format_short_time(std::int64_t seconds) {
    return format_with(seconds, "%e %b, %l:%M %p");
}

std::string tense_label(const LanguagePack& pack, std::string_view key) {
    for (const auto& [id, name] : pack.tense_labels) {
        if (id == key) return name;
    }
    return std::string(key);
}

std::string capitalise(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    }
    return text;
}

std::string end_reason_label(std::int64_t ended_at, const std::string& reason) {
    if (ended_at == 0) {
        return "In progress";
    }
    static const std::array<std::pair<const char*, const char*>, 5> kReasons{{
        {"student_end", "Ended by student"},
        {"timer",       "Time ran out"},
        {"disconnect",  "Left before the end"},
        {"crash",       "Server restarted"},
        {"quota",       "Out of questions for today"},
    }};
    for (const auto& [stored, shown] : kReasons) {
        if (reason == stored) return shown;
    }
    return reason.empty() ? "Ended" : reason;
}

std::string short_length(int seconds) {
    if (seconds % 60 == 0) {
        return std::to_string(seconds / 60) + " min";
    }
    return std::to_string(seconds / 60) + " min " +
           std::to_string(seconds % 60) + " s";
}

std::string format_length(int seconds, int standard) {
    if (seconds == 0) {
        return "Standard (" + short_length(standard) + ")";
    }
    return short_length(seconds);
}

std::string pluralise(int count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

std::string format_join_code(const std::string& code) {
    if (code.size() != 8) {
        return code;
    }
    return code.substr(0, 4) + "-" + code.substr(4);
}

}  // namespace sim
