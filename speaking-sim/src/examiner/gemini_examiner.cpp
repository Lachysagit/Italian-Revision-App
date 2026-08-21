#include "sim/examiner/gemini_examiner.hpp"

#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>
#include "crow/json.h"

namespace sim {

namespace {

// The one value most likely to drift. Lift to config later if needed.
constexpr const char* kModel = "gemini-2.5-flash";

// cpp-httplib defaults to 300 s for both connect and read. A worker is held for
// the whole of respond(), so one stalled call parked a pool thread for five
// minutes; with a small pool that is the entire server refusing turns. These
// bound it to something a student would actually wait through.
constexpr time_t kConnectTimeoutSeconds = 10;
constexpr time_t kReadTimeoutSeconds = 60;
constexpr time_t kWriteTimeoutSeconds = 10;

// Sent as the sole user turn when the history carries nothing but the system
// prompt. generateContent rejects a request whose contents array is absent or
// empty, so the opening question of every exam used to fail with HTTP 400.
constexpr const char* kOpeningTurnText = "Inizia l'esame.";

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
    unsigned content_index = 0;
    // crow wvalue::operator[] takes an unsigned index. This was std::size_t,
    // so every subscript narrowed 64 bits to 32 (MSVC C4267). The history is
    // three turns at most, so unsigned is the honest type rather than a cast.

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

    if (content_index == 0) {
        body["contents"][0]["role"] = "user";
        body["contents"][0]["parts"][0]["text"] = kOpeningTurnText;
        // The Start turn builds a history of [System] alone, so the loop above
        // emitted no contents at all and the request carried only a
        // system_instruction. Gemini requires a non-empty contents array.
    }

    if (!system_text.empty()) {
        body["system_instruction"]["parts"][0]["text"] = system_text;
    }

    httplib::Client cli("https://generativelanguage.googleapis.com");
    cli.set_connection_timeout(kConnectTimeoutSeconds);
    cli.set_read_timeout(kReadTimeoutSeconds);
    cli.set_write_timeout(kWriteTimeoutSeconds);
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
        throw std::runtime_error("Gemini returned unreadable JSON");
    }

    if (!parsed.has("candidates")) {
        throw std::runtime_error("Gemini response carried no candidates field");
        // crow rvalue::operator[] throws "cannot find key: candidates" on a
        // missing key, which is true but says nothing about which call failed.
        // Every hop below is checked for the same reason.
    }

    const crow::json::rvalue& candidates = parsed["candidates"];
    if (candidates.t() != crow::json::type::List || candidates.size() == 0) {
        throw std::runtime_error("Gemini returned no candidates");
    }

    const crow::json::rvalue& candidate = candidates[0];
    if (!candidate.has("content") || !candidate["content"].has("parts") ||
        candidate["content"]["parts"].size() == 0) {
        std::string reason = "unspecified";
        if (candidate.has("finishReason")) {
            reason = std::string(candidate["finishReason"].s());
        }
        throw std::runtime_error(
            "Gemini returned a candidate with no text, finishReason=" + reason);
        // A candidate stopped by SAFETY, RECITATION or MAX_TOKENS carries no
        // content.parts at all. The old chained subscript walked straight into
        // it and surfaced a crow key error, hiding the actual cause.
    }

    const crow::json::rvalue& part = candidate["content"]["parts"][0];
    if (!part.has("text") || part["text"].t() != crow::json::type::String) {
        throw std::runtime_error("Gemini part carried no text");
    }

    return std::string(part["text"].s());
    // .s() hands back an r_string pointing into the buffer owned by `parsed`.
    // Converting to std::string here copies it out before that buffer dies with
    // this frame.
}

}  // namespace sim
