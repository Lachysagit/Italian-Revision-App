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

// The one value most likely to drift. Lift to config later if needed.
//
// TEMPORARY - TESTING ONLY. REVERT TO "gemini-3.6-flash" BEFORE SHIPPING.
// gemini-3.6-flash is the ship target; it is parked at 20/20 requests per day,
// and gemini-3.5-flash draws on a separate daily bucket, so this is the swap
// that buys a testable examiner until that quota resets. Reverting this line
// means revisiting kThinkingLevel below too - the two were changed together.
constexpr const char* kModel = "gemini-3.5-flash";

constexpr const char* kHost = "https://generativelanguage.googleapis.com";

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

// The request carried no generationConfig at all, so both of the values below
// were left at the model's defaults. ListModels reports outputTokenLimit
// 65536 for gemini-3.6-flash, and that is the ceiling an unbounded request is
// free to run to - which is what the 8K output spike is. An examiner follow-up
// is two sentences: the observed reply for this exact system prompt is 30-35
// tokens, and Italian tokenises at roughly 1.5 tokens per word, so 512 is
// about six times the honest length of a reply and 128 times below the default
// ceiling. The headroom is deliberate: this model has thinking enabled, thought
// tokens are charged against maxOutputTokens, and a cap sized to the visible
// reply alone would be spent thinking before the first word of Italian was
// emitted.
constexpr int kMaxOutputTokens = 512;

// The 3.x models take thinkingConfig.thinkingLevel, an enum string, and reject
// the 2.5-series thinkingBudget integer. Thought tokens are billed against
// maxOutputTokens, so every token spent reasoning is a token unavailable to
// the reply - and asking one on-topic follow-up needs no reasoning worth the
// name. Measured on gemini-3.5-flash, same prompt and cap: no thinkingConfig
// spent 295 thought tokens, "low" spent 194, "minimal" spent 0.
//
// "low" rather than "minimal" because support is per-model, not per-family,
// and the two models either side of this one disagree: gemini-3.5-flash
// accepts "minimal", gemini-3.7-flash rejects it with HTTP 400 "Thinking level
// MINIMAL is not supported for this model". "low" is accepted by both. All
// four names pass the enum check on gemini-3.6-flash itself, but that check
// only proves the name is in the ThinkingLevel enum - whether the model
// backend supports the level is a second, per-model check that runs only on
// generateContent, and generateContent could not be reached to test it while
// the daily quota was exhausted. So this is the value with evidence on both
// sides rather than the lowest one that might work.
//
// Worth one call once quota resets: if "minimal" is accepted by this model it
// takes thoughts to zero, and the per-call usage line below reports the
// thoughts count directly, so the difference is visible without instrumenting
// anything further.
//
// TEMPORARY - TESTING ONLY, PAIRED WITH THE kModel SWAP ABOVE.
// The reasoning above picked "low" as the value safe across 3.5, 3.6 and 3.7.
// That constraint is suspended while kModel is pinned to gemini-3.5-flash,
// which is measured to accept "minimal" and to spend 0 thought tokens on it.
// WHEN REVERTING kModel TO "gemini-3.6-flash": put this back to "low", or
// re-test "minimal" against 3.6 first. Whether 3.6 supports MINIMAL is still
// unverified - it passes the enum check but the per-model backend check runs
// only on generateContent, which was unreachable at the time. Shipping
// "minimal" against 3.6 untested risks HTTP 400 on every examiner turn.
constexpr const char* kThinkingLevel = "minimal";

// The model default is 1.0. Lower tightens adherence to the two standing
// instructions in the prompt - one question at a time, simple Italian - which
// is the same drift that inflates output length. Not lower than this on
// purpose: an examiner that opens with a byte-identical question in every
// session is worse practice than one that varies it, so this buys obedience
// without buying determinism. 0.5 rather than 0.6 because crow's dump() writes
// a double to 17 significant digits: 0.6 goes on the wire as
// 0.59999999999999998, which Gemini parses back to the same value but which is
// unreadable in a request log. 0.5 is exactly representable and dumps as "0.5".
constexpr double kTemperature = 0.5;

// Operator-side only. The frame the student sees stays the fixed string the
// server sends; this exists so a failed turn in the log says which of the
// failures it was without the reader decoding a status by hand.
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

    // No failure below is retried, and that is the policy rather than an
    // omission. A retry re-sends the whole request, so it costs another
    // generate_content_free_tier_requests unit against a per-project,
    // per-model daily cap of 20 - the budget being spent is call count, not
    // tokens. A 4xx cannot come back different for a byte-identical body, so
    // retrying one spends quota to receive the same error twice, and retrying
    // a 429 spends exactly the quota it is complaining about. Every failure
    // here throws once, and the server's catch turns it into the send_error
    // frame and stops. If a bounded retry is ever added it belongs on the two
    // TRANSIENT statuses and the network-error branch only, never on a 4xx.

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
        // the body carries Gemini's own error.message, which names the exact
        // quota metric on a 429 and the offending field on a 400. It already
        // travelled inside the exception text, but only reached stderr behind
        // the server's generic "turn failed" prefix, with the status it was
        // conditional on never called out at all.
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
        // content.parts at all. The old chained subscript walked straight into
        // it and surfaced a crow key error, hiding the actual cause.
    }

    if (parsed.has("usageMetadata")) {
        const crow::json::rvalue& usage = parsed["usageMetadata"];
        std::cerr << "gemini usage: prompt "
                  << usage_field(usage, "promptTokenCount") << ", output "
                  << usage_field(usage, "candidatesTokenCount") << ", thoughts "
                  << usage_field(usage, "thoughtsTokenCount") << " (cap "
                  << kMaxOutputTokens << ")\n";
        // one line per call, alongside the server's turn timings. thoughts is
        // the number that decides whether the cap is sized right: output plus
        // thoughts is what the cap bounds and what the dashboard bills as
        // output, so a thoughts figure crowding the cap is the signal to raise
        // it before replies start arriving truncated.
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
