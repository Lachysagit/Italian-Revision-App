#include "sim/examiner/gemini_examiner.hpp"

#include <cstdint>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>
#include "crow/json.h"

namespace sim {

namespace {

// TEMPORARY - REVERT TO "gemini-3.6-flash" BEFORE SHIPPING; 3.6 is parked at
// 20/20 requests per day. Revert kThinkingLevel below in the same edit.
constexpr const char* kModel = "gemini-3.5-flash";

constexpr const char* kHost = "https://generativelanguage.googleapis.com";

// cpp-httplib defaults to 300 s and respond() holds a worker for its whole
// duration, so one stalled call parked a pool thread for five minutes.
constexpr time_t kConnectTimeoutSeconds = 10;
constexpr time_t kReadTimeoutSeconds = 60;
constexpr time_t kWriteTimeoutSeconds = 10;

// Sole user turn when the history is system-prompt-only: generateContent
// rejects an absent or empty contents array, so the opener failed with 400.
constexpr const char* kOpeningTurnText = "Inizia l'esame.";

// Left unset this runs to the model's 65536 ceiling, which is the 8K output
// spike. A reply is 30-35 tokens; the headroom covers billed thought tokens.
constexpr int kMaxOutputTokens = 512;

// 3.x takes thinkingLevel (enum), not the 2.5-series thinkingBudget integer.
// TEMPORARY - restore "low", the 3.5/3.6/3.7-safe value, when reverting kModel.
constexpr const char* kThinkingLevel = "minimal";

// Under the 1.0 default to tighten prompt adherence, but not to 0: a varied
// opening question is better practice. 0.5 dumps exactly, 0.6 does not.
constexpr double kTemperature = 0.5;

// Operator-side only; the student still sees the server's fixed string. Names
// the failure in the log so nobody has to decode a status by hand.
const char* failure_kind(int status) {
    switch (status) {
        case 400:
            return "BAD_REQUEST - malformed body, a retry cannot succeed";
        case 401:
        case 403:
            return "AUTH - key rejected, a retry cannot succeed";
        case 404:
            return "NOT_FOUND - model id or path does not exist, a retry "
                   "cannot succeed";
        case 429:
            return "QUOTA - rate limited, DO NOT retry, every attempt counts "
                   "against RPD/RPM";
        case 500:
        case 503:
            return "TRANSIENT - server side, retryable in principle";
        default:
            return "UNEXPECTED";
    }
}

std::int64_t usage_field(const crow::json::rvalue& usage, const char* key) {
    return usage.has(key) ? usage[key].i() : 0;
    // absent rather than zero is the normal case for thoughtsTokenCount, and
    // rvalue::operator[] throws on a missing key rather than returning null
}

const char* gemini_role(Role role) {
    // Gemini's contents array knows only "user" and "model".
    // A System turn is handled separately and never reaches here.
    return role == Role::Examiner ? "model" : "user";
}

}  // namespace

GeminiExaminer::GeminiExaminer(std::string api_key)
    : api_key_(std::move(api_key)) {} // constructor

std::string GeminiExaminer::respond(const std::vector<Turn>& history) {
    crow::json::wvalue body;
    std::string system_text;
    unsigned content_index = 0;
    // crow wvalue::operator[] takes unsigned; std::size_t narrowed 64 bits to
    // 32 (MSVC C4267). History is three turns, so unsigned is the honest type.

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
        // The Start turn's history is [System] alone, so the loop emitted no
        // contents. Gemini requires a non-empty contents array.
    }

    if (!system_text.empty()) {
        body["system_instruction"]["parts"][0]["text"] = system_text;
    }

    body["generationConfig"]["maxOutputTokens"] = kMaxOutputTokens;
    body["generationConfig"]["temperature"] = kTemperature;
    body["generationConfig"]["thinkingConfig"]["thinkingLevel"] = kThinkingLevel;

    // One keep-alive Client per worker thread: httplib defaults keep_alive_ to
    // false, and releases socket_mutex_ before send/recv so one cannot be shared.
    thread_local httplib::Client cli = [] {
        httplib::Client c(kHost);
        c.set_keep_alive(true);
        c.set_connection_timeout(kConnectTimeoutSeconds);
        c.set_read_timeout(kReadTimeoutSeconds);
        c.set_write_timeout(kWriteTimeoutSeconds);
        return c;
    }();
    const std::string path =
        std::string("/v1beta/models/") + kModel + ":generateContent";
    const httplib::Headers headers = {{"x-goog-api-key", api_key_}};

    httplib::Result res =
        cli.Post(path, headers, body.dump(), "application/json");

    // Nothing below is retried, by policy: a retry costs another request
    // against a 20/day cap and a 4xx cannot differ for a byte-identical body.

    if (!res) {
        std::cerr << "gemini failure: NETWORK - "
                  << httplib::to_string(res.error())
                  << " (no request reached the API, so no quota was spent)\n";
        throw std::runtime_error(
            "Gemini request failed: " + httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        std::cerr << "gemini failure: HTTP " << res->status << " "
                  << failure_kind(res->status) << "\n  body: " << res->body
                  << '\n';
        // The body carries Gemini's own error.message - the exact quota metric
        // on a 429, the offending field on a 400 - which the generic prefix hid.
        throw std::runtime_error(
            "Gemini HTTP " + std::to_string(res->status) + ": " + res->body);
    }

    crow::json::rvalue parsed = crow::json::load(res->body);
    if (!parsed) {
        throw std::runtime_error("Gemini returned unreadable JSON");
    }

    if (!parsed.has("candidates")) {
        throw std::runtime_error("Gemini response carried no candidates field");
        // crow rvalue::operator[] throws "cannot find key: candidates", which
        // says nothing about which call failed. Every hop below is checked too.
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
        if (reason == "MAX_TOKENS") {
            std::cerr << "gemini failure: MAX_TOKENS - the whole "
                      << kMaxOutputTokens
                      << "-token output budget was consumed before any text "
                         "was emitted (thought tokens are charged against it). "
                         "Raise kMaxOutputTokens.\n";
        }
        throw std::runtime_error(
            "Gemini returned a candidate with no text, finishReason=" + reason);
        // A candidate stopped by SAFETY, RECITATION or MAX_TOKENS carries no
        // content.parts, and a chained subscript hides that behind a key error.
    }

    if (parsed.has("usageMetadata")) {
        const crow::json::rvalue& usage = parsed["usageMetadata"];
        std::cerr << "gemini usage: prompt "
                  << usage_field(usage, "promptTokenCount") << ", output "
                  << usage_field(usage, "candidatesTokenCount") << ", thoughts "
                  << usage_field(usage, "thoughtsTokenCount") << " (cap "
                  << kMaxOutputTokens << ")\n";
        // One line per call. thoughts decides whether the cap is sized right:
        // crowding it means raising kMaxOutputTokens before replies truncate.
    }

    const crow::json::rvalue& part = candidate["content"]["parts"][0];
    if (!part.has("text") || part["text"].t() != crow::json::type::String) {
        throw std::runtime_error("Gemini part carried no text");
    }

    return std::string(part["text"].s());
    // .s() points into the buffer owned by `parsed`; converting to std::string
    // copies it out before that buffer dies with this frame.
}

}  // namespace sim
