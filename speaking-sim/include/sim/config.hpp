#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sim {

enum class ExaminerBackend {
    Gemini,
    Hailo,
};

struct GeminiKeyOption {
    std::string name;
    std::string key;
};

enum class AudioInput {
    Gemini,
    //the captured audio goes to the examiner, which transcribes and replies in
    //one call. Whisper stays loaded and takes over if that call fails
    Whisper,
};

// How long each class of record is kept. The windows, and the reasoning behind
// each one, are written up in docs/compliance/data-retention.md - change them
// there and here together, because that file is what goes to the school.
//
// 0 means "keep forever", which is allowed for a development box and REFUSED
// once AUTH_REQUIRED is on: A3 requires records be kept no longer than
// necessary, and "forever" is not a period.
struct RetentionPolicy {
    int transcript_days = 90;
    //attempt_turns rows - the student's own words and the examiner's. The
    //shortest window of the four, because this is the most sensitive text the
    //system holds
    int attempt_days = 455;
    //exam_attempts and everything that cascades from it. Must be >= both of
    //the windows above and below, because it is their parent row: deleting an
    //attempt deletes its turns and its safety events with it
    int safety_event_days = 365;
    //safety_events rows. Deliberately longer than the transcripts they refer
    //to: the flag that an incident happened outlives the practice data, and
    //the table holds no utterance to begin with
    int inactive_account_days = 0;
    //users untouched for this long, with everything that cascades from them.
    //Off by default: an account is deleted when a school says so, not because
    //a student had a quiet term
};

enum class SafetyMode {
    Off,
    //no screening at all. Development only, and refused outright once
    //AUTH_REQUIRED is on: an unscreened exam is not a degraded exam, it is no
    //exam (compliant-flow.md, checkpoint 0b)
    Local,
    //the wordlist layer alone. The offline build, and everything that still
    //works on the Pi with the network cable out
    Azure,
    //wordlist first, then Content Safety and Prompt Shields in Australia East
};

enum class AdjudicatorMode {
    Off,
    //no semantic reasoning pass. Every verdict the chain reaches stands,
    //which is the behaviour before this existed and the default
    Examiner,
    //flagged text is sent back to the examiner backend for a reasoning pass.
    //Runs on whatever the examiner runs on, so it follows the examiner to
    //Australia East rather than needing its own compliance story
};

enum class AudioCodec {
    Flac,
    Wav,
    //both are lossless and carry the same samples. Wav is the buffer plus a
    //44-byte header and needs no encoder; flac halves the bytes on the wire
};

struct Config {
    std::string gemini_api_key;
    //first entry of gemini_api_keys, kept for callers that only want a default
    std::vector<GeminiKeyOption> gemini_api_keys;
    //named keys parsed from GEMINI_API_KEYS, the pool a settings picker chooses from
    std::string gemini_model;
    //the model id. Configurable because it has moved three times (3.5, 3.6,
    //3.8) and each move used to be a recompile
    std::string gemini_thinking_level;
    std::string gemini_opening_thinking_level;
    //low, medium or high. 3.8 rejects the "minimal" that 3.5 took, so an
    //unchecked value here is a 400 on every turn - validated at startup
    ExaminerBackend examiner_backend = ExaminerBackend::Gemini;
    AudioInput audio_input = AudioInput::Gemini;
    AudioCodec audio_codec = AudioCodec::Flac;
    std::string translate_api_key;
    //Google Cloud key for the translate box, separate from the Gemini pool so
    //casual lookups do not spend the examiner's rationed daily requests
    std::string hailo_ollama_url;
    std::string whisper_model_path;
    std::string piper_model_path;
    //the italian voice, kept singular so an .env written before multi-language
    //still selects it. LANGUAGE_VOICES overrides per language on top
    std::vector<std::pair<std::string, std::string>> language_voices;
    //language id -> piper voice path, parsed from LANGUAGE_VOICES. Empty means
    //every language keeps the voice built into language.cpp
    std::string google_client_id;
    std::string google_client_secret;
    //the OAuth client this server signs users in with. Required once
    //auth_required is on, and checked at startup rather than on the first
    //sign-in attempt
    std::string public_origin;
    //the origin a browser reaches this server on. One value drives three
    //things: the redirect_uri handed to Google, whether the session cookie is
    //marked Secure, and ws:// against wss://. Changing deployment is this line
    std::vector<std::string> teacher_emails;
    //TEACHER_EMAILS, lowercased at load. Whoever signs in with one of these
    //may create classes; everyone else is a student until a teacher adds them
    bool auth_required = false;
    //false while sign-in is being built: the pages and routes exist, nothing is
    //gated, and an exam still runs for a browser that has never signed in

    SafetyMode safety_mode = SafetyMode::Local;
    //SAFETY_MODE. Local by default so a build that says nothing about safety
    //is screened rather than open
    std::string safety_wordlist_dir;
    bool safety_fail_closed = true;
    //SAFETY_FAIL_CLOSED. A screening layer that throws halts the turn. Only
    //ever turned off for the offline build, which has no layer that can throw
    std::string content_safety_endpoint;
    std::string content_safety_key;
    //empty in the deployed configuration: the relay's managed identity holds
    //Cognitive Services User on the resource, and a key that does not exist
    //cannot be leaked in a log line
    int content_safety_halt_severity = 2;
    //0, 2, 4 or 6 on the four-level scale. Deliberately stricter than the
    //service default, because the users are minors
    bool safety_shield_prompts = true;

    AdjudicatorMode adjudicator_mode = AdjudicatorMode::Off;
    //SAFETY_ADJUDICATOR. Off by default: the system is correct without the
    //reasoning pass, and switching it on is a decision someone signs off
    std::vector<std::string> adjudicate_categories;
    //SAFETY_ADJUDICATE_CATEGORIES. Which categories a reasoning pass may
    //review. Never includes jailbreak or profanity, whatever is written here
    bool adjudicate_self_harm = false;
    //SAFETY_ADJUDICATE_SELF_HARM. Separate from the list above so self-harm
    //cannot be switched on by editing a comma-separated string. Even when
    //true, a verdict two detectors agreed on is still untouchable

    std::string database_path;
    //accounts, classes and exam history. A relative path resolves against the
    //working directory, which is speaking-sim/ like every other asset path here

    int free_daily_questions = 5;
    int paid_daily_questions = 200;
    //FREE_DAILY_QUESTIONS and PAID_DAILY_QUESTIONS: examiner questions per
    //account per local day. Paid means a licence on the account or on one of
    //its classes, or a teacher account. Listening is never metered
    int exam_duration_seconds = 300;
    //EXAM_DURATION_SECONDS. The server's own clock, started when the opening
    //question goes out; the browser is told the figure and counts down to it

    RetentionPolicy retention;
    //TRANSCRIPT_RETENTION_DAYS, ATTEMPT_RETENTION_DAYS,
    //SAFETY_EVENT_RETENTION_DAYS and INACTIVE_ACCOUNT_RETENTION_DAYS

    std::uint16_t port = 8080;

    std::size_t worker_threads = 2;
 
};

Config load_config();

}  // namespace sim
