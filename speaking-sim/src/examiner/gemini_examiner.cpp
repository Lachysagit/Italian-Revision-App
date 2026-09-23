#include "sim/examiner/gemini_examiner.hpp"

#include "sim/topics.hpp"

#include <chrono>
#include <cstddef>
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

// The model id and both thinking levels now come from config (GEMINI_MODEL,
// GEMINI_THINKING_LEVEL, GEMINI_OPENING_THINKING_LEVEL). They were constants
// here through three model moves, and each one meant a recompile.

constexpr const char* kHost = "https://generativelanguage.googleapis.com";

// cpp-httplib defaults to 300 s and respond() holds a worker for its whole
// duration, so one stalled call parked a pool thread for five minutes.
constexpr time_t kConnectTimeoutSeconds = 10;
constexpr time_t kReadTimeoutSeconds = 60;
constexpr time_t kWriteTimeoutSeconds = 10;


// generateContent rejects an empty contents array, so the opening turn needs a
// user message of its own. Session supplies it, from the exam language's own
// LanguagePack: the text used to be a compiled-in Italian constant here, which
// put one language inside a backend that should not know about any, and made
// the opening turn the one place a second language could not reach.
//
// It still deliberately carries NO instructions. It once spelled out how long
// the opening question should be, which made it a fourth place competing with
// the prompt files over the same rule - and the one place nobody thinks to
// look. All of that lives in each language's examiner_first.txt, which Session
// hands over as the System turn for exactly this request.

// Left unset this runs to the model's 65536 ceiling, which is the 8K output
// spike. A reply is 50-70 tokens now that they run to two-to-four sentences,
// up from the 30-35 of the one-clause era; the headroom covers billed thought
// tokens, so this stays where it is.
constexpr int kMaxOutputTokens = 512;

// Deprecated on 3.x: the guidance is to leave temperature at its 1.0 default,
// and Google warn that lowering it can cause looping or degraded output. Kept
// for now because the docs say "strip" rather than "rejected", so whether 3.8
// actually refuses it is worth learning from a real response - a 400 names the
// offending field in the body logged below. Drop both constants if it does.
//
// Under the 1.0 default to tighten prompt adherence, but not to 0: a varied
// opening question is better practice. 0.5 dumps exactly, 0.6 does not.
constexpr double kTemperature = 0.5;


constexpr double kOpeningTemperature = 1.0;

// The opening turn takes its own thinking level, and the floor is deliberate.
// Reasoning collapses the sampling distribution: the model thinks its way to
// the one canonical easiest-beginner-question and temperature then only picks
// between wordings of it, which is why 1.0 above was not buying any variety.
// Nothing on the opening turn needs thought - there is no answer to respond to
// - and the floor also takes latency off the turn the student waits through
// before the exam starts.
//
// The API has no "none". 3.5 and 3.6 took minimal/low/medium/high; 3.8 dropped
// "minimal" and errors on it, so "low" is now the floor. config.cpp checks the
// value at startup, because anything outside the set is a 400 on every turn.

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

// One keep-alive Client per worker thread: httplib defaults keep_alive_ to
// false, and releases socket_mutex_ before send/recv so one cannot be shared.
// Hoisted out of respond() so prewarm() warms the very same connection - a
// second client here would open a second socket and warm nothing.
httplib::Client& client() {
    thread_local httplib::Client cli = [] {
        httplib::Client c(kHost);
        c.set_keep_alive(true);
        c.set_connection_timeout(kConnectTimeoutSeconds);
        c.set_read_timeout(kReadTimeoutSeconds);
        c.set_write_timeout(kWriteTimeoutSeconds);
        return c;
    }();
    return cli;
}

// The tag is an enum the API enforces, so the model cannot invent one of its
// own and no amount of prompt drift can widen the set.
void apply_response_schema(crow::json::wvalue& body) {
    crow::json::wvalue& schema = body["generationConfig"]["responseSchema"];
    schema["type"] = "OBJECT";
    schema["properties"]["reply"]["type"] = "STRING";
    schema["properties"]["topic"]["type"] = "STRING";
    for (unsigned i = 0; i < kTopicTags.size(); ++i) {
        schema["properties"]["topic"]["enum"][i] =
            std::string(kTopicTags[i]);
    }
    schema["required"][0] = "reply";
    schema["required"][1] = "topic";
    schema["propertyOrdering"][0] = "reply";
    schema["propertyOrdering"][1] = "topic";
    //reply first, so the Italian is generated before the tag rather than after

    body["generationConfig"]["responseMimeType"] = "application/json";
}

// Back into the text-plus-tag shape the rest of the program reads, so
// server.cpp's topic_tag() and the Hailo path stay as they are.
std::string flatten_structured_reply(const std::string& text) {
    crow::json::rvalue parsed = crow::json::load(text);
    if (!parsed || !parsed.has("reply") || !parsed.has("topic") ||
        parsed["reply"].t() != crow::json::type::String ||
        parsed["topic"].t() != crow::json::type::String) {
        return text;
        //a schema hiccup degrades to the free-text path rather than throwing
    }
    return std::string(parsed["reply"].s()) + "\n[topic: " +
           std::string(parsed["topic"].s()) + "]";
}

const char* gemini_role(Role role) {
    // Gemini's contents array knows only "user" and "model".
    // A System turn is handled separately and never reaches here.
    return role == Role::Examiner ? "model" : "user";
}

}  // namespace

GeminiExaminer::GeminiExaminer(std::vector<GeminiKeyOption> keys,
                               GeminiSettings settings)
    : keys_(std::move(keys)), settings_(std::move(settings)) {} // constructor

const std::string& GeminiExaminer::key_for(const std::string& gemini_key_name) const {
    if (!gemini_key_name.empty()) {
        for (const GeminiKeyOption& option : keys_) {
            if (option.name == gemini_key_name) return option.key;
        }
        //an unrecognised name (stale client, edited .env) falls through to the
        //default below rather than failing the whole request
    }
    static const std::string empty;
    return keys_.empty() ? empty : keys_.front().key;
}

void GeminiExaminer::prewarm() {
    // DNS, the TCP connect and the TLS handshake together measure 150-490ms
    // against this host, and every one of those milliseconds used to land on
    // the first turn a worker thread served. Doing it here spends them while
    // nobody is waiting.
    //
    // A models-list GET rather than a generateContent POST, deliberately: it
    // sits in a different quota bucket, so warming the socket costs nothing
    // against the 20/day request cap that the real calls are rationed by.
    const auto started = std::chrono::steady_clock::now();
    const httplib::Headers headers = {{"x-goog-api-key", key_for("")}};
    const httplib::Result res = client().Get("/v1beta/models", headers);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count();

    if (!res) {
        std::cerr << "examiner prewarm: NETWORK - "
                  << httplib::to_string(res.error())
                  << ", first turn will pay the handshake" << std::endl;
        return;
        //never throws. A cold connection is a slow turn, not a broken server,
        //and startup must not hinge on the network being up
    }
    std::cerr << "examiner prewarm: HTTP " << res->status << " in " << ms
              << "ms, connection open" << std::endl;
    //the status is logged rather than checked: even a 4xx completed a full
    //handshake, which is the only thing this call was ever after
}

std::string GeminiExaminer::respond(const std::vector<Turn>& history,
                                     const std::string& gemini_key_name) {
    const std::string& api_key = key_for(gemini_key_name);
    crow::json::wvalue body;
    std::string system_text;
    unsigned content_index = 0;
    // crow wvalue::operator[] takes unsigned; std::size_t narrowed 64 bits to
    // 32 (MSVC C4267). History is three turns, so unsigned is the honest type.

    bool examiner_has_spoken = false;

    for (const Turn& turn : history) {
        if (turn.role == Role::System) {
            if (!system_text.empty()) system_text += "\n\n";
            system_text += turn.text;
            continue;
        }
        examiner_has_spoken = examiner_has_spoken || turn.role == Role::Examiner;
        body["contents"][content_index]["role"] = gemini_role(turn.role);
        body["contents"][content_index]["parts"][0]["text"] = turn.text;
        ++content_index;
    }

    const bool opening_turn = !examiner_has_spoken;
    // No examiner turn in the history means nothing has been asked yet, so this
    // request is the opening question. It used to be detected as "the loop
    // emitted no contents", because Session sent the System prompt alone and
    // this function bolted on a hardcoded Italian user turn to satisfy Gemini's
    // non-empty contents rule. Session now sends that opening turn itself, in
    // the exam's own language, so the count is never zero and the old test
    // would silently never fire - taking kOpeningTemperature with it.

    if (!system_text.empty()) {
        body["system_instruction"]["parts"][0]["text"] = system_text;
    }

    body["generationConfig"]["maxOutputTokens"] = kMaxOutputTokens;
    body["generationConfig"]["temperature"] =
        opening_turn ? kOpeningTemperature : kTemperature;
    //the opening turn, not a count: it is the one request with no real
    //conversation behind it, which is the whole condition
    body["generationConfig"]["thinkingConfig"]["thinkingLevel"] =
        opening_turn ? settings_.opening_thinking_level
                     : settings_.thinking_level;
    apply_response_schema(body);

    const std::string path =
        "/v1beta/models/" + settings_.model + ":generateContent";
    const httplib::Headers headers = {{"x-goog-api-key", api_key}};

    httplib::Result res =
        client().Post(path, headers, body.dump(), "application/json");

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

    return flatten_structured_reply(std::string(part["text"].s()));
    // .s() points into the buffer owned by `parsed`; converting to std::string
    // copies it out before that buffer dies with this frame.
}

}  // namespace sim
