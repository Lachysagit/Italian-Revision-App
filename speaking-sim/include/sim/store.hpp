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

    //Phase 0 opens the database and creates the schema; nothing calls it yet.
    //The accessors that follow arrive with the phases that need them: users and
    //cookie sessions with OAuth, attempts and turns with exam history.

private:
    void exec(const char* sql);
    void migrate();
    void reconcile_crashed_attempts();

    std::recursive_mutex m_;
    sqlite3* db_ = nullptr;
};

}  // namespace sim
