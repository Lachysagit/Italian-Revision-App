#pragma once

#include <string>

namespace sim {

// Piper pronounces markdown and whisper's [BLANK_AUDIO] annotations, so both
// are stripped before anything reaches the student. ASCII markers only; UTF-8
// passes through untouched.

// Markdown markers, bracketed spans, whitespace runs.
std::string clean_for_speech(const std::string& text);

// As above, plus parenthesised spans. May return empty, which the caller
// already treats as "didn't catch that".
std::string clean_transcript(const std::string& text);

// The [topic: ...] tag, lowercased, or empty when there is no usable tag.
std::string topic_tag(const std::string& text);

}  // namespace sim
