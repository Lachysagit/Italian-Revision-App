#include "sim/text_clean.hpp"

#include <cctype>
#include <cstddef>
#include <string>

namespace sim {

namespace {

// Brackets are handled as spans below. Backslash is here because an escaped
// marker leaves its backslash behind once the marker is gone.
bool is_markup(char ch) {
    switch (ch) {
        case '*': case '_': case '`': case '#': case '~':
        case '|': case '\\': case '^':
            return true;
        default:
            return false;
    }
}

bool is_space(char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}


// Returns open itself when the bracket never closes, so an unmatched '[' is a
// stray character rather than the start of a span that eats the rest.
std::size_t span_end(const std::string& text, std::size_t open, char closer) {
    const std::size_t found = text.find(closer, open + 1);
    return found == std::string::npos ? open : found;
}

// Collapses and trims, so removing a marker leaves no double space behind.
std::string collapse_spaces(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    bool pending_space = false;
    for (const char ch : text) {
        if (is_space(ch)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(ch);
        //written only once a character follows, which trims both ends
    }
    return out;
}

std::string strip(const std::string& text, bool drop_parenthesised) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];

        if (ch == '[' || ch == '<' || (drop_parenthesised && ch == '(')) {
            const char closer = ch == '[' ? ']' : (ch == '<' ? '>' : ')');
            const std::size_t end = span_end(text, i, closer);
            if (end != i) {
                i = end;
                continue;
                //contents included: an annotation was never spoken
            }
        }

        if (ch == ']' || ch == '>' || (drop_parenthesised && ch == ')')) {
            continue;
            //an unmatched closer, better dropped than read out
        }

        if (is_markup(ch)) {
            continue;
        }

        // A '-' is a list marker at the start of a line and a hyphen anywhere
        // else, where it has to stay.
        if (ch == '-' && i + 1 < text.size() && is_space(text[i + 1])) {
            const std::size_t newline = out.find_last_of('\n');
            const std::size_t line_start =
                newline == std::string::npos ? 0 : newline + 1;
            if (out.find_first_not_of(" \t", line_start) == std::string::npos) {
                continue;
            }
        }

        out.push_back(ch);
    }

    return collapse_spaces(out);
}

}  // namespace

std::string clean_for_speech(const std::string& text) {
    std::string cleaned = strip(text, false);
    //parentheses are kept: an examiner's aside is still something it chose to say
    return cleaned.empty() ? text : cleaned;
    //a reply that was all markers goes out raw rather than silent
}

std::string topic_tag(const std::string& text) {
    // Short enough that a stray "[topic:" cannot pass a paragraph off as a tag.
    constexpr std::size_t kMaxTopicLength = 40;

    static const std::string kMarker = "[topic:";

    std::string lowered;
    lowered.reserve(text.size());
    for (const char ch : text) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    //the model writes [Topic:] and [TOPIC:] often enough to matter

    const std::size_t marker = lowered.find(kMarker);
    if (marker == std::string::npos) {
        return {};
    }

    const std::size_t open = marker + kMarker.size();
    const std::size_t close = lowered.find(']', open);
    if (close == std::string::npos || close - open > kMaxTopicLength) {
        return {};
        //no tag, so the turn counts against the topic already running
    }

    std::string topic = lowered.substr(open, close - open);

    const std::size_t first = topic.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
        //"[topic: ]", opened and never filled in
    }
    const std::size_t last = topic.find_last_not_of(" \t");
    return topic.substr(first, last - first + 1);
}

std::string clean_transcript(const std::string& text) {
    return strip(text, true);
    //empty is legitimate: audio that was nothing but [BLANK_AUDIO] had no answer
}

}  // namespace sim
