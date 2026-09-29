#include "sim/safety/azure_safety.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>

#include <httplib.h>
#include "crow/json.h"

namespace sim {

namespace {

constexpr const char* kAnalyzePath =
    "/contentsafety/text:analyze?api-version=2024-09-01";
constexpr const char* kShieldPath =
    "/contentsafety/text:shieldPrompt?api-version=2024-09-01";

// Tighter than translate's, because this call sits between the student
// finishing their answer and the examiner being asked anything. Two of these
// run per turn; 15 s each would double the worst-case turn.
constexpr time_t kConnectTimeoutSeconds = 5;
constexpr time_t kReadTimeoutSeconds = 8;
constexpr time_t kWriteTimeoutSeconds = 5;

// Same thread_local discipline as gemini_examiner.cpp and translate.cpp:
// httplib releases socket_mutex_ around send and recv, so one shared client
// across workers is not safe.
httplib::Client& client(const std::string& endpoint) {
    thread_local httplib::Client cli = [&] {
        httplib::Client c(endpoint);
        c.set_keep_alive(true);
        c.set_connection_timeout(kConnectTimeoutSeconds);
        c.set_read_timeout(kReadTimeoutSeconds);
        c.set_write_timeout(kWriteTimeoutSeconds);
        return c;
    }();
    return cli;
}

httplib::Headers headers_for(const AzureSafety::Options& options) {
    if (!options.api_key.empty()) {
        return {{"Ocp-Apim-Subscription-Key", options.api_key}};
    }
    return {{"Authorization", "Bearer " + options.bearer_token}};
}

// Azure's own category names, kept verbatim rather than mapped to friendlier
// ones: the string in the safety_events row should be the string the service
// returned, so an incident report and the service's own logs line up.
std::string lowercase_category(const std::string& category) {
    std::string out;
    for (char ch : category) {
        if (std::isupper(static_cast<unsigned char>(ch)) && !out.empty()) {
            out.push_back('_');
        }
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(ch))));
    }
    return out;
    //"SelfHarm" -> "self_harm", so it matches the wordlist layer's category
}

}  // namespace

AzureSafety::AzureSafety(Options options) : options_(std::move(options)) {
    if (options_.endpoint.empty()) {
        throw std::runtime_error(
            "CONTENT_SAFETY_ENDPOINT is required once SAFETY_MODE=azure");
    }
}

bool AzureSafety::available() const {
    return healthy_.load();
}

void AzureSafety::prewarm() {
    try {
        const SafetyVerdict verdict = analyse("buongiorno");
        healthy_.store(verdict.action == SafetyAction::Allow);
    } catch (const std::exception& e) {
        healthy_.store(false);
        std::cerr << "safety prewarm failed: " << e.what() << '\n';
        //not fatal here. main() checks available() after prewarming and
        //refuses to start with AUTH_REQUIRED on and safety unavailable, so the
        //failure is reported once with context rather than thrown from a
        //worker thread at startup
    }
}

SafetyVerdict AzureSafety::screen(const std::string& text,
                                  SafetyStage stage,
                                  const std::string& language_id) {
    (void)language_id;
    //the service detects language itself; the field stays in the interface
    //because the wordlist layer needs it

    SafetyVerdict verdict;
    verdict.text = text;
    if (text.empty()) return verdict;

    if (stage == SafetyStage::StudentSpeech && options_.shield_prompts) {
        SafetyVerdict shielded = shield(text);
        if (shielded.action != SafetyAction::Allow) return shielded;
    }

    SafetyVerdict analysed = analyse(text);
    if (analysed.action == SafetyAction::Allow) return analysed;

    // Self-harm is the one category that is not a discipline matter. A student
    // who says something worrying in an Italian exam has still said it, and the
    // turn owes them a person rather than a refusal.
    if (analysed.category == "self_harm") {
        analysed.action = SafetyAction::Escalate;
    }
    return analysed;
}

SafetyVerdict AzureSafety::analyse(const std::string& text) {
    crow::json::wvalue body;
    body["text"] = text;
    body["categories"][0] = "Hate";
    body["categories"][1] = "SelfHarm";
    body["categories"][2] = "Sexual";
    body["categories"][3] = "Violence";
    body["outputType"] = "FourSeverityLevels";

    const httplib::Result res = client(options_.endpoint)
        .Post(kAnalyzePath, headers_for(options_), body.dump(),
              "application/json");

    if (!res) {
        throw std::runtime_error("content safety unreachable: " +
                                 httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        throw std::runtime_error("content safety HTTP " +
                                 std::to_string(res->status) + ": " + res->body);
        //the body is the service's own error, never the student's text, so it
        //is safe in the operator log
    }

    const crow::json::rvalue parsed = crow::json::load(res->body);
    if (!parsed || !parsed.has("categoriesAnalysis")) {
        throw std::runtime_error("content safety returned no analysis");
    }

    SafetyVerdict verdict;
    verdict.text = text;
    verdict.detector = "content_safety";

    for (const crow::json::rvalue& entry : parsed["categoriesAnalysis"]) {
        const int severity = entry.has("severity") ? entry["severity"].i() : 0;
        if (severity < options_.halt_severity) continue;
        if (severity <= verdict.severity) continue;

        verdict.action = SafetyAction::Halt;
        verdict.severity = severity;
        verdict.category = lowercase_category(entry["category"].s());
        //highest severity wins, so a reply that trips two categories is
        //recorded under the worse one
    }

    return verdict;
}

SafetyVerdict AzureSafety::shield(const std::string& text) {
    crow::json::wvalue body;
    body["userPrompt"] = text;
    body["documents"] = crow::json::wvalue::list();

    const httplib::Result res = client(options_.endpoint)
        .Post(kShieldPath, headers_for(options_), body.dump(),
              "application/json");

    if (!res) {
        throw std::runtime_error("prompt shield unreachable: " +
                                 httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        throw std::runtime_error("prompt shield HTTP " +
                                 std::to_string(res->status));
    }

    SafetyVerdict verdict;
    verdict.text = text;
    verdict.detector = "prompt_shield";

    const crow::json::rvalue parsed = crow::json::load(res->body);
    if (parsed && parsed.has("userPromptAnalysis") &&
        parsed["userPromptAnalysis"].has("attackDetected") &&
        parsed["userPromptAnalysis"]["attackDetected"].b()) {
        verdict.action = SafetyAction::Halt;
        verdict.category = "jailbreak";
        verdict.severity = options_.halt_severity;
    }
    return verdict;
}

}  // namespace sim
