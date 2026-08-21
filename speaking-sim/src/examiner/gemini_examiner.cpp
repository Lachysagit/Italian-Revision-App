#include "sim/examiner/gemini_examiner.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include <httplib.h>
#include "crow/json.h"

namespace sim {

namespace {

// The one value most likely to drift. Lift to config later if needed.
constexpr const char* kModel = "gemini-2.5-flash";

const char* gemini_role(Role role) {
    // Gemini's contents array knows only "user" and "model".
    // A System turn is handled separately and never reaches here.
    return role == Role::Examiner ? "model" : "user";
}

}  // namespace

GeminiExaminer::GeminiExaminer(std::string api_key)
    : api_key_(std::move(api_key)) {}

std::string GeminiExaminer::respond(const std::vector<Turn>& history) {
    crow::json::wvalue body;
    std::string system_text;
    std::size_t content_index = 0;

    for (const Turn& turn : history) {
        if (turn.role == Role::System) {
            if (!system_text.empty()) system_text += "\n\n";
            system_text += turn.text;
            continue;
        }
        body["contents"][content_index]["role"] = gemini_role(turn.role);
        body["contents"][content_index]["parts"][0]["text"] = turn.text;
        ++content_index;
    }

    if (!system_text.empty()) {
        body["system_instruction"]["parts"][0]["text"] = system_text;
    }

    httplib::Client cli("https://generativelanguage.googleapis.com");
    const std::string path =
        std::string("/v1beta/models/") + kModel + ":generateContent";
    const httplib::Headers headers = {{"x-goog-api-key", api_key_}};

    httplib::Result res =
        cli.Post(path, headers, body.dump(), "application/json");

    if (!res) {
        throw std::runtime_error(
            "Gemini request failed: " + httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        throw std::runtime_error(
            "Gemini HTTP " + std::to_string(res->status) + ": " + res->body);
    }

    crow::json::rvalue parsed = crow::json::load(res->body);
    if (!parsed) {
        throw std::runtime_error("Gemini returned unparseable JSON");
    }

    const auto& candidates = parsed["candidates"];
    if (candidates.size() == 0) {
        throw std::runtime_error("Gemini returned no candidates");
    }

    return candidates[0]["content"]["parts"][0]["text"].s();
}

}  // namespace sim