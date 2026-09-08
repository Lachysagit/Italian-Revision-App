#pragma once

#include <string>

namespace sim {

// One sentence in, its translation out, via Google Cloud Translation v2.
//
// A free function rather than an Interface* class like the examiner and TTS:
// there is one backend and one call site, so there is nothing to inject and
// nothing to swap. Throws std::runtime_error on a network failure, a non-200
// status, or a response that does not carry a translation.
std::string translate_text(const std::string& api_key,
                           const std::string& text,
                           const std::string& source_lang,
                           const std::string& target_lang);

}  // namespace sim
