#include "sim/store.hpp"

#include "sim/auth/google_oauth.hpp"

#include <openssl/sha.h>
#include <sqlite3.h>

#include <iostream>
#include <stdexcept>

namespace sim {

namespace {

//the whole schema, applied once under PRAGMA user_version. A migration
//framework would be more machinery than a single-file database for one
//classroom needs: version 1 is everything, and a version 2 would be an ALTER
//block guarded the same way.
constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  display_name TEXT NOT NULL DEFAULT '',
  picture_url  TEXT NOT NULL DEFAULT '',
  is_teacher   INTEGER NOT NULL DEFAULT 0,
  year_level    TEXT NOT NULL DEFAULT '',
  subject_level TEXT NOT NULL DEFAULT '',
  preferred_language TEXT NOT NULL DEFAULT '',
  onboarded_at  INTEGER,
  daily_attempt_cap INTEGER,
  created_at INTEGER NOT NULL,
  last_seen_at INTEGER
);

CREATE TABLE oauth_identities (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  provider TEXT NOT NULL DEFAULT 'google',
  subject  TEXT NOT NULL,
  UNIQUE(provider, subject)
);

CREATE TABLE profile_change_requests (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  old_year TEXT NOT NULL DEFAULT '', old_level TEXT NOT NULL DEFAULT '',
  new_year TEXT NOT NULL DEFAULT '', new_level TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL,
  requested_at INTEGER NOT NULL,
  decided_by INTEGER REFERENCES users(id), decided_at INTEGER
);
CREATE INDEX pcr_pending ON profile_change_requests(status, user_id);

CREATE TABLE auth_sessions (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash BLOB NOT NULL UNIQUE,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  user_agent TEXT NOT NULL DEFAULT ''
);
CREATE INDEX auth_sessions_user ON auth_sessions(user_id);

CREATE TABLE classes (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  language_id TEXT NOT NULL DEFAULT '',
  year_level    TEXT NOT NULL DEFAULT '',
  subject_level TEXT NOT NULL DEFAULT '',
  gemini_key_name TEXT NOT NULL DEFAULT '',
  daily_attempt_cap INTEGER,
  join_code TEXT UNIQUE,
  owner_id INTEGER NOT NULL REFERENCES users(id),
  created_at INTEGER NOT NULL,
  archived_at INTEGER
);

CREATE TABLE class_members (
  class_id INTEGER NOT NULL REFERENCES classes(id) ON DELETE CASCADE,
  user_id  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  role TEXT NOT NULL,
  added_at INTEGER NOT NULL,
  PRIMARY KEY (class_id, user_id)
);

CREATE TABLE class_invites (
  id INTEGER PRIMARY KEY,
  class_id INTEGER NOT NULL REFERENCES classes(id) ON DELETE CASCADE,
  email TEXT NOT NULL COLLATE NOCASE,
  role TEXT NOT NULL DEFAULT 'student',
  invited_by INTEGER NOT NULL REFERENCES users(id),
  created_at INTEGER NOT NULL,
  claimed_at INTEGER,
  UNIQUE(class_id, email)
);

CREATE TABLE rooms (
  id INTEGER PRIMARY KEY,
  class_id INTEGER REFERENCES classes(id) ON DELETE CASCADE,
  host_id  INTEGER NOT NULL REFERENCES users(id),
  join_code TEXT UNIQUE,
  language_id TEXT NOT NULL,
  turn_mode TEXT NOT NULL DEFAULT 'buzz',
  state TEXT NOT NULL DEFAULT 'lobby',
  created_at INTEGER NOT NULL, ended_at INTEGER,
  team_score REAL, team_score_json TEXT
);

CREATE TABLE room_members (
  room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  joined_at INTEGER NOT NULL, left_at INTEGER,
  individual_score REAL, individual_score_json TEXT,
  PRIMARY KEY (room_id, user_id)
);

CREATE TABLE room_turns (
  id INTEGER PRIMARY KEY,
  room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  turn_index INTEGER NOT NULL,
  role TEXT NOT NULL,
  text TEXT NOT NULL,
  topic TEXT NOT NULL DEFAULT '',
  speaker_user_id   INTEGER REFERENCES users(id),
  nominated_user_id INTEGER REFERENCES users(id),
  created_at INTEGER NOT NULL,
  UNIQUE(room_id, turn_index)
);

CREATE TABLE exam_attempts (
  id INTEGER PRIMARY KEY,
  user_id  INTEGER REFERENCES users(id) ON DELETE CASCADE,
  class_id INTEGER REFERENCES classes(id),
  room_id  INTEGER REFERENCES rooms(id),
  language_id TEXT NOT NULL,
  gemini_key_name TEXT NOT NULL DEFAULT '',
  started_at INTEGER NOT NULL,
  ended_at INTEGER,
  end_reason TEXT,
  turn_count INTEGER NOT NULL DEFAULT 0,
  examiner_calls INTEGER NOT NULL DEFAULT 0,
  score_overall REAL, score_json TEXT, scored_at INTEGER
);
CREATE INDEX attempts_user_time  ON exam_attempts(user_id, started_at DESC);
CREATE INDEX attempts_class_time ON exam_attempts(class_id, started_at DESC);

CREATE TABLE attempt_turns (
  id INTEGER PRIMARY KEY,
  attempt_id INTEGER NOT NULL REFERENCES exam_attempts(id) ON DELETE CASCADE,
  turn_index INTEGER NOT NULL,
  role TEXT NOT NULL,
  text TEXT NOT NULL,
  topic TEXT NOT NULL DEFAULT '',
  stt_ms INTEGER, examiner_ms INTEGER, tts_ms INTEGER,
  speaker_user_id INTEGER REFERENCES users(id),
  created_at INTEGER NOT NULL,
  UNIQUE(attempt_id, turn_index)
);

CREATE TABLE key_usage_daily (
  day TEXT NOT NULL,
  gemini_key_name TEXT NOT NULL,
  call_count INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (day, gemini_key_name)
);
)SQL";
//rooms is declared before exam_attempts because the latter references it.
//SQLite would accept either order, but the readable order is the one where a
//table exists before it is pointed at

}  // namespace

Store::Store(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string message =
            db_ != nullptr ? sqlite3_errmsg(db_) : "could not allocate a handle";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("sqlite open failed: " + message);
    }

    exec("PRAGMA journal_mode=WAL");
    //readers stop blocking the writer, which matters mainly because the sqlite3
    //CLI will be open against this file while the server is running
    exec("PRAGMA busy_timeout=3000");
    exec("PRAGMA foreign_keys=ON");
    //already compiled in by SQLITE_DEFAULT_FOREIGN_KEYS, set again so the build
    //flag and the runtime cannot drift apart

    migrate();
    reconcile_crashed_attempts();
}

Store::~Store() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void Store::exec(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

void Store::migrate() {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error("sqlite could not read user_version");
    }
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (version < 1) {
        exec("BEGIN");
        try {
            exec(kSchemaV1);
            exec("PRAGMA user_version=3");
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
        std::cerr << "store: created schema version 3\n";
        return;
        //a fresh database gets the current shape in one step and skips the
        //migration below, which only exists to carry an older file forward
    }

    if (version < 2) {
        exec("BEGIN");
        try {
            exec("ALTER TABLE users ADD COLUMN preferred_language "
                 "TEXT NOT NULL DEFAULT ''");
            exec("PRAGMA user_version=2");
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
        std::cerr << "store: migrated schema to version 2\n";
    }

    if (version < 3) {
        exec("BEGIN");
        try {
            exec("ALTER TABLE classes ADD COLUMN year_level "
                 "TEXT NOT NULL DEFAULT ''");
            exec("ALTER TABLE classes ADD COLUMN subject_level "
                 "TEXT NOT NULL DEFAULT ''");
            exec("PRAGMA user_version=3");
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
        std::cerr << "store: migrated schema to version 3\n";
        //a class is defined by the cohort it teaches, so the two levels
        //sit on the class row beside the language it already carried
    }
}

void Store::reconcile_crashed_attempts() {
    std::lock_guard<std::recursive_mutex> lock(m_);

    exec("UPDATE exam_attempts "
         "SET ended_at = CAST(strftime('%s','now') AS INTEGER), "
         "    end_reason = 'crash' "
         "WHERE ended_at IS NULL");
    //a killed server leaves attempts open forever, and a dashboard showing them
    //as permanently in progress reads as a bug in the exam rather than in the
    //shutdown. Cheap enough to run unconditionally at every startup

    const int changed = sqlite3_changes(db_);
    if (changed > 0) {
        std::cerr << "store: closed " << changed
                  << " attempt(s) left open by a previous run\n";
    }
}

namespace {

//sessions are looked up by the hash of the cookie, never by the cookie itself:
//a copied database file then contains no usable tokens.
std::string sha256_raw(const std::string& text) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(),
           digest);
    return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

User read_user_row(sqlite3_stmt* stmt) {
    const auto text = [stmt](int col) {
        const unsigned char* value = sqlite3_column_text(stmt, col);
        return value ? std::string(reinterpret_cast<const char*>(value))
                     : std::string();
    };
    User user;
    user.id = sqlite3_column_int64(stmt, 0);
    user.email = text(1);
    user.display_name = text(2);
    user.picture_url = text(3);
    user.is_teacher = sqlite3_column_int(stmt, 4) != 0;
    user.year_level = text(5);
    user.subject_level = text(6);
    user.preferred_language = text(7);
    user.onboarded = sqlite3_column_type(stmt, 8) != SQLITE_NULL;
    return user;
}

constexpr const char* kUserColumns =
    "users.id, users.email, users.display_name, users.picture_url, "
    "users.is_teacher, users.year_level, users.subject_level, "
    "users.preferred_language, users.onboarded_at";
//table qualified: user_for_auth_token joins auth_sessions, which has its own
//user_id, and an unqualified "id" is ambiguous there
//one list, so read_user_row's column indices cannot drift from the queries

}  // namespace

User Store::upsert_google_user(const GoogleProfile& profile, bool is_teacher) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    exec("BEGIN IMMEDIATE");
    try {
        std::int64_t user_id = 0;

        sqlite3_stmt* find = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT user_id FROM oauth_identities "
            "WHERE provider = 'google' AND subject = ?",
            -1, &find, nullptr);
        sqlite3_bind_text(find, 1, profile.subject.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(find) == SQLITE_ROW) {
            user_id = sqlite3_column_int64(find, 0);
        }
        sqlite3_finalize(find);

        if (user_id == 0) {
            sqlite3_stmt* by_email = nullptr;
            sqlite3_prepare_v2(db_, "SELECT id FROM users WHERE email = ?",
                               -1, &by_email, nullptr);
            sqlite3_bind_text(by_email, 1, profile.email.c_str(), -1,
                              SQLITE_TRANSIENT);
            if (sqlite3_step(by_email) == SQLITE_ROW) {
                user_id = sqlite3_column_int64(by_email, 0);
            }
            sqlite3_finalize(by_email);
        }
        //the email fallback is what lets a roster entry and a first sign-in
        //meet on one row. email is COLLATE NOCASE, so the capitals a teacher
        //typed do not open a second account

        if (user_id == 0) {
            sqlite3_stmt* insert = nullptr;
            sqlite3_prepare_v2(db_,
                "INSERT INTO users (email, display_name, picture_url, "
                " is_teacher, created_at, last_seen_at) "
                "VALUES (?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER), "
                "        CAST(strftime('%s','now') AS INTEGER))",
                -1, &insert, nullptr);
            sqlite3_bind_text(insert, 1, profile.email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert, 2, profile.display_name.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(insert, 3, profile.picture_url.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_int(insert, 4, is_teacher ? 1 : 0);
            const int rc = sqlite3_step(insert);
            sqlite3_finalize(insert);
            if (rc != SQLITE_DONE) {
                throw std::runtime_error(std::string("user insert: ") +
                                         sqlite3_errmsg(db_));
            }
            user_id = sqlite3_last_insert_rowid(db_);
        } else {
            sqlite3_stmt* update = nullptr;
            sqlite3_prepare_v2(db_,
                "UPDATE users SET display_name = ?, picture_url = ?, "
                " is_teacher = max(is_teacher, ?), "
                " last_seen_at = CAST(strftime('%s','now') AS INTEGER) "
                "WHERE id = ?",
                -1, &update, nullptr);
            sqlite3_bind_text(update, 1, profile.display_name.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(update, 2, profile.picture_url.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_int(update, 3, is_teacher ? 1 : 0);
            sqlite3_bind_int64(update, 4, user_id);
            sqlite3_step(update);
            sqlite3_finalize(update);
            //max() so dropping an address from TEACHER_EMAILS does not quietly
            //demote somebody mid-term. Taking the role away is a deliberate act
        }

        sqlite3_stmt* link = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO oauth_identities (user_id, provider, subject) "
            "VALUES (?, 'google', ?)",
            -1, &link, nullptr);
        sqlite3_bind_int64(link, 1, user_id);
        sqlite3_bind_text(link, 2, profile.subject.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(link);
        sqlite3_finalize(link);

        exec("COMMIT");

        auto user = user_by_id(user_id);
        if (!user) {
            throw std::runtime_error("user vanished immediately after upsert");
        }
        return *user;
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

std::optional<User> Store::user_by_id(std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kUserColumns + " FROM users WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("user_by_id prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, user_id);

    std::optional<User> found;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = read_user_row(stmt);
    }
    sqlite3_finalize(stmt);
    return found;
}

void Store::set_profile(std::int64_t user_id,
                        const std::string& year_level,
                        const std::string& subject_level,
                        const std::string& preferred_language) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE users SET year_level = ?, subject_level = ?, "
            " preferred_language = ?, "
            " onboarded_at = COALESCE(onboarded_at, "
            "                         CAST(strftime('%s','now') AS INTEGER)) "
            "WHERE id = ?",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("set_profile prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    //COALESCE so onboarded_at records the first time only: it is the answer to
    //"has this account finished signing up", not "when was the profile last
    //touched", and a later edit must not restart that clock

    sqlite3_bind_text(stmt, 1, year_level.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, subject_level.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, preferred_language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, user_id);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("set_profile step: ") +
                                 sqlite3_errmsg(db_));
    }
}

bool Store::is_in_any_class(std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT 1 FROM class_members WHERE user_id = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("is_in_any_class prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, user_id);

    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

std::int64_t Store::create_class(std::int64_t owner_id,
                                 const std::string& name,
                                 const std::string& language_id,
                                 const std::string& year_level,
                                 const std::string& subject_level) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    exec("BEGIN IMMEDIATE");
    try {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO classes "
                "(name, language_id, year_level, subject_level, owner_id, "
                " created_at) "
                "VALUES (?, ?, ?, ?, ?, "
                "        CAST(strftime('%s','now') AS INTEGER))",
                -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("create_class prepare: ") +
                                     sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, language_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, year_level.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, subject_level.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, owner_id);

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(std::string("create_class step: ") +
                                     sqlite3_errmsg(db_));
        }
        const std::int64_t class_id = sqlite3_last_insert_rowid(db_);

        sqlite3_stmt* member = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO class_members (class_id, user_id, role, added_at) "
                "VALUES (?, ?, 'teacher', "
                "        CAST(strftime('%s','now') AS INTEGER))",
                -1, &member, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("create_class member: ") +
                                     sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(member, 1, class_id);
        sqlite3_bind_int64(member, 2, owner_id);

        const int member_rc = sqlite3_step(member);
        sqlite3_finalize(member);
        if (member_rc != SQLITE_DONE) {
            throw std::runtime_error(std::string("create_class member step: ") +
                                     sqlite3_errmsg(db_));
        }
        //the owner is a member as well as the owner: classes_for_user reads
        //class_members, so skipping this would hide the class from the very
        //teacher who just made it

        exec("COMMIT");
        return class_id;
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

std::vector<ClassSummary> Store::classes_for_user(std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT classes.id, classes.name, classes.language_id, "
            "       classes.year_level, classes.subject_level, mine.role, "
            "       (SELECT COUNT(*) FROM class_members "
            "         WHERE class_members.class_id = classes.id) "
            "  FROM classes "
            "  JOIN class_members AS mine ON mine.class_id = classes.id "
            " WHERE mine.user_id = ? AND classes.archived_at IS NULL "
            " ORDER BY classes.created_at DESC, classes.id DESC",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("classes_for_user prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, user_id);

    const auto text = [stmt](int col) {
        const unsigned char* value = sqlite3_column_text(stmt, col);
        return value ? std::string(reinterpret_cast<const char*>(value))
                     : std::string();
    };

    std::vector<ClassSummary> found;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ClassSummary summary;
        summary.id = sqlite3_column_int64(stmt, 0);
        summary.name = text(1);
        summary.language_id = text(2);
        summary.year_level = text(3);
        summary.subject_level = text(4);
        summary.role = text(5) == "teacher" ? ClassRole::Teacher
                                            : ClassRole::Student;
        summary.member_count = sqlite3_column_int(stmt, 6);
        found.push_back(std::move(summary));
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Store::has_created_class(std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT 1 FROM classes WHERE owner_id = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("has_created_class prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, user_id);

    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
    //owner_id rather than class_members, and archived classes count: a teacher
    //who made a class and archived it has still been asked once
}

std::string Store::create_auth_session(std::int64_t user_id,
                                       const std::string& user_agent) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string token = auth::random_token(32);
    const std::string hash = sha256_raw(token);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO auth_sessions "
            "(user_id, token_hash, created_at, expires_at, user_agent) "
            "VALUES (?, ?, CAST(strftime('%s','now') AS INTEGER), "
            "        CAST(strftime('%s','now','+30 days') AS INTEGER), ?)",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("create_auth_session prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_blob(stmt, 2, hash.data(), static_cast<int>(hash.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, user_agent.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("create_auth_session step: ") +
                                 sqlite3_errmsg(db_));
    }
    return token;
}

std::optional<User> Store::user_for_auth_token(const std::string& token) {
    if (token.empty()) return std::nullopt;

    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string hash = sha256_raw(token);
    const std::string sql =
        std::string("SELECT ") + kUserColumns +
        " FROM users JOIN auth_sessions ON auth_sessions.user_id = users.id "
        " WHERE auth_sessions.token_hash = ? "
        "   AND auth_sessions.expires_at > CAST(strftime('%s','now') AS INTEGER)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("user_for_auth_token prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_bind_blob(stmt, 1, hash.data(), static_cast<int>(hash.size()),
                      SQLITE_TRANSIENT);

    std::optional<User> found;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = read_user_row(stmt);
    }
    sqlite3_finalize(stmt);
    return found;
}

void Store::delete_auth_session(const std::string& token) {
    if (token.empty()) return;

    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string hash = sha256_raw(token);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM auth_sessions WHERE token_hash = ?",
                       -1, &stmt, nullptr);
    sqlite3_bind_blob(stmt, 1, hash.data(), static_cast<int>(hash.size()),
                      SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::int64_t Store::begin_attempt(std::optional<std::int64_t> user_id,
                                  const std::string& language_id,
                                  const std::string& gemini_key_name) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO exam_attempts "
            "(user_id, language_id, gemini_key_name, started_at) "
            "VALUES (?, ?, ?, CAST(strftime('%s','now') AS INTEGER))",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("begin_attempt prepare: ") +
                                 sqlite3_errmsg(db_));
    }

    if (user_id.has_value()) {
        sqlite3_bind_int64(stmt, 1, *user_id);
    } else {
        sqlite3_bind_null(stmt, 1);
        //anonymous until sign-in lands. The column is nullable for exactly this
        //phase, and tightens once every attempt has a user behind it
    }
    sqlite3_bind_text(stmt, 2, language_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, gemini_key_name.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("begin_attempt step: ") +
                                 sqlite3_errmsg(db_));
    }
    return sqlite3_last_insert_rowid(db_);
}

void Store::record_turn(std::int64_t attempt_id,
                        int turn_index,
                        const std::string& role,
                        const std::string& text,
                        const std::string& topic,
                        long long stt_ms,
                        long long examiner_ms,
                        long long tts_ms) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO attempt_turns "
            "(attempt_id, turn_index, role, text, topic, "
            " stt_ms, examiner_ms, tts_ms, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, "
            "        CAST(strftime('%s','now') AS INTEGER))",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("record_turn prepare: ") +
                                 sqlite3_errmsg(db_));
    }

    sqlite3_bind_int64(stmt, 1, attempt_id);
    sqlite3_bind_int(stmt, 2, turn_index);
    sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, stt_ms);
    sqlite3_bind_int64(stmt, 7, examiner_ms);
    sqlite3_bind_int64(stmt, 8, tts_ms);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("record_turn step: ") +
                                 sqlite3_errmsg(db_));
    }

    exec("UPDATE exam_attempts SET turn_count = turn_count + 1 "
         "WHERE id = " + std::to_string(attempt_id));
    //attempt_id is an integer this process generated, never client input, so
    //there is nothing here for a bind to protect against
}

void Store::note_examiner_call(std::int64_t attempt_id,
                               const std::string& gemini_key_name) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    exec("UPDATE exam_attempts SET examiner_calls = examiner_calls + 1 "
         "WHERE id = " + std::to_string(attempt_id));

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO key_usage_daily (day, gemini_key_name, call_count) "
            "VALUES (date('now','localtime'), ?, 1) "
            "ON CONFLICT(day, gemini_key_name) "
            "DO UPDATE SET call_count = call_count + 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("note_examiner_call prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    //localtime rather than UTC: a daily cap has to roll over at the student's
    //midnight, not at whatever hour UTC lands on

    sqlite3_bind_text(stmt, 1, gemini_key_name.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("note_examiner_call step: ") +
                                 sqlite3_errmsg(db_));
    }
}

void Store::end_attempt(std::int64_t attempt_id, const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE exam_attempts "
            "SET ended_at = CAST(strftime('%s','now') AS INTEGER), "
            "    end_reason = ? "
            "WHERE id = ? AND ended_at IS NULL",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("end_attempt prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    //ended_at IS NULL makes this idempotent: the socket closing after the timer
    //already ended the exam must not relabel a clean finish as a disconnect

    sqlite3_bind_text(stmt, 1, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, attempt_id);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("end_attempt step: ") +
                                 sqlite3_errmsg(db_));
    }
}

}  // namespace sim
