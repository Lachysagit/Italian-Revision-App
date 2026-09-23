#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace sim {

//accounts, classes and exam history. One sqlite3 handle guarded by one mutex:
//SQLITE_THREADSAFE=1 makes a single call safe, but not a prepare/bind/step
//sequence and not a transaction, so every public method below takes the lock
//for its whole body. Crow's socket threads and the worker pool both land here
//and the write volume is a few rows per turn, so contention is not a concern.
//
//Held as a member of Server, like LanguageRegistry. Unlike that registry it is
//mutable rather than const-after-startup, which is why Session must never hold
//a pointer to it: Session stays a pure state object and Server does the I/O.

enum class ClassRole {
    Teacher,
    Student,
};

struct User {
    std::int64_t id = 0;
    std::string email;
    std::string display_name;
    std::string picture_url;
    bool is_teacher = false;
    std::string year_level;
    //'9'..'12', for class grouping and reporting. Does not route keys
    std::string subject_level;
    //beginners|continuers|advanced|extension - this is what selects the key pool
    bool onboarded = false;
    //false until a level has been chosen. No exam may start before it is true,
    //which is what stops a brand new account spending anything
};

struct GoogleProfile {
    std::string subject;   //Google's `sub`, stable forever - the join key
    std::string email;
    std::string display_name;
    std::string picture_url;
};

class Store {
public:
    explicit Store(const std::string& path);
    //opens, applies the schema and reconciles attempts left open by a crash.
    //Throws on failure: a server that cannot record anything should not start
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // ---- users and sign-in -----------------------------------------------

    User upsert_google_user(const GoogleProfile& profile, bool is_teacher);
    //matched on (provider, subject) first and on email second, so a teacher who
    //invited an address before that student ever signed in ends up on the same
    //row rather than creating a duplicate account beside the invite

    std::optional<User> user_by_id(std::int64_t user_id);

    // ---- cookie sessions -------------------------------------------------

    std::string create_auth_session(std::int64_t user_id,
                                    const std::string& user_agent);
    //returns the raw token for the cookie. Only its SHA-256 is stored, so the
    //database file cannot be read for live logins

    std::optional<User> user_for_auth_token(const std::string& token);
    //also the websocket's check, so it must stay cheap: one indexed lookup

    void delete_auth_session(const std::string& token);

    // ---- exam attempts ---------------------------------------------------
    //the accessors for users, classes and cookie sessions arrive with the
    //phases that need them. These are the ones exam history needs.

    std::int64_t begin_attempt(std::optional<std::int64_t> user_id,
                               const std::string& language_id,
                               const std::string& gemini_key_name);
    //user_id is nullopt until sign-in exists. Returns 0 if the row could not be
    //written, which every caller below treats as "do not persist this attempt"
    //rather than as an error worth failing the exam over

    void record_turn(std::int64_t attempt_id,
                     int turn_index,
                     const std::string& role,
                     const std::string& text,
                     const std::string& topic,
                     long long stt_ms,
                     long long examiner_ms,
                     long long tts_ms);
    //also bumps the attempt's turn_count, so a dashboard does not have to count
    //rows to show how long an exam ran

    void note_examiner_call(std::int64_t attempt_id,
                            const std::string& gemini_key_name);
    //per call rather than per attempt: this is what least-used-today key
    //selection will read once pools exist

    void end_attempt(std::int64_t attempt_id, const std::string& reason);
    //WHERE ended_at IS NULL, so a disconnect arriving after a timer has already
    //closed the attempt cannot overwrite the more specific reason

private:
    void exec(const char* sql);
    void exec(const std::string& sql) { exec(sql.c_str()); }
    void migrate();
    void reconcile_crashed_attempts();

    std::recursive_mutex m_;
    sqlite3* db_ = nullptr;
};

}  // namespace sim
