#include "sim/store.hpp"

#include "sim/auth/google_oauth.hpp"

#include <openssl/sha.h>
#include <sqlite3.h>

#include <openssl/rand.h>

#include <cctype>
#include <iostream>
#include <stdexcept>

namespace sim {

namespace {

//the whole schema, applied once under PRAGMA user_version. A migration
//framework would be more machinery than a single-file database for one
//classroom needs: version 1 is everything, and until this ships the way to
//change it is to edit here and delete the database file.
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
  default_plan_id INTEGER REFERENCES exam_plans(id) ON DELETE SET NULL,
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
  plan_id   INTEGER REFERENCES exam_plans(id) ON DELETE SET NULL,
  plan_name TEXT NOT NULL DEFAULT '',
  plan_json TEXT,
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

-- Exam plans: what a teacher asks the examiner to cover. A plan belongs to one
-- class; the class may name one as the default every exam for it follows, and
-- students may pick any visible plan by name. The rows are the editable plan;
-- an attempt keeps its own frozen copy in exam_attempts.plan_json, so editing a
-- plan never rewrites what an earlier exam was asked to do.
CREATE TABLE exam_plans (
  id INTEGER PRIMARY KEY,
  class_id INTEGER NOT NULL REFERENCES classes(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  duration_seconds INTEGER,
  require_opinion INTEGER NOT NULL DEFAULT 1,
  paraphrase_ok   INTEGER NOT NULL DEFAULT 0,
  visible INTEGER NOT NULL DEFAULT 1,
  created_by INTEGER NOT NULL REFERENCES users(id),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  archived_at INTEGER
);
CREATE INDEX exam_plans_class ON exam_plans(class_id);

CREATE TABLE exam_plan_topics (
  plan_id INTEGER NOT NULL REFERENCES exam_plans(id) ON DELETE CASCADE,
  topic_group TEXT NOT NULL,
  position INTEGER NOT NULL,
  PRIMARY KEY (plan_id, topic_group)
);

CREATE TABLE exam_plan_questions (
  id INTEGER PRIMARY KEY,
  plan_id INTEGER NOT NULL REFERENCES exam_plans(id) ON DELETE CASCADE,
  text TEXT NOT NULL,
  topic_group TEXT NOT NULL DEFAULT '',
  placement TEXT NOT NULL DEFAULT 'any',
  position INTEGER NOT NULL
);
CREATE INDEX exam_plan_questions_plan ON exam_plan_questions(plan_id, position);

CREATE TABLE exam_plan_tenses (
  plan_id INTEGER NOT NULL REFERENCES exam_plans(id) ON DELETE CASCADE,
  tense TEXT NOT NULL,
  min_count INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (plan_id, tense)
);

-- What each turn of an exam did. kind is 'tense' for now; source says who
-- decided it - 'model' is the examiner's own label from its structured reply,
-- 'rules' the deterministic check in tense_rules.cpp - so a report can show
-- both and a disagreement stays visible rather than being averaged away.
CREATE TABLE turn_features (
  attempt_id INTEGER NOT NULL REFERENCES exam_attempts(id) ON DELETE CASCADE,
  turn_index INTEGER NOT NULL,
  kind  TEXT NOT NULL,
  value TEXT NOT NULL,
  source TEXT NOT NULL,
  PRIMARY KEY (attempt_id, turn_index, kind, value, source)
);

-- The plan's must-ask questions, per attempt, with whether each was asked. The
-- text is copied in so the record survives the plan being edited or deleted.
CREATE TABLE attempt_required_questions (
  attempt_id  INTEGER NOT NULL REFERENCES exam_attempts(id) ON DELETE CASCADE,
  question_id INTEGER NOT NULL,
  text TEXT NOT NULL,
  topic_group TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'pending',
  turn_index INTEGER,
  PRIMARY KEY (attempt_id, question_id)
);

-- Paid access. A licence covers one account ('user') or every student in one
-- class ('class'), which is how a school licence is sold: per class, invoiced,
-- with no card details held anywhere. Granted from the command line for now
-- (--grant-licence); a payment provider would write the same rows.
CREATE TABLE licences (
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL,
  target_id INTEGER NOT NULL,
  starts_at INTEGER NOT NULL,
  ends_at INTEGER NOT NULL,
  note TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  revoked_at INTEGER
);
CREATE INDEX licences_target ON licences(kind, target_id);

-- What each account has spent today, per metered feature. day is the server's
-- local date, so the allowance resets at local midnight - run the server with
-- TZ=Australia/Sydney and that is a student's midnight too.
CREATE TABLE usage_daily (
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  day TEXT NOT NULL,
  feature TEXT NOT NULL,
  used INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (user_id, day, feature)
);

CREATE INDEX class_members_user ON class_members(user_id);
CREATE INDEX class_invites_open ON class_invites(email) WHERE claimed_at IS NULL;
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
            exec("PRAGMA user_version=1");
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
        std::cerr << "store: created schema version 1\n";
    }
    //one version, one CREATE: the database is still only ever made from
    //scratch, so an older file is deleted rather than carried forward
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

    exec("UPDATE attempt_required_questions SET status = 'missed' "
         "WHERE status = 'pending' AND attempt_id IN "
         "(SELECT id FROM exam_attempts WHERE ended_at IS NOT NULL)");
    //the set questions of an exam the crash ended were not asked either
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
            "SELECT 1 FROM class_members "
            "WHERE user_id = ? AND role = 'student' LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("is_in_any_class prepare: ") +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, user_id);

    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
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
                                  std::optional<std::int64_t> class_id,
                                  const std::string& language_id,
                                  const std::string& gemini_key_name) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO exam_attempts "
            "(user_id, class_id, language_id, gemini_key_name, started_at) "
            "VALUES (?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER))",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("begin_attempt prepare: ") +
                                 sqlite3_errmsg(db_));
    }

    if (user_id.has_value()) {
        sqlite3_bind_int64(stmt, 1, *user_id);
    } else {
        sqlite3_bind_null(stmt, 1);
        //a browser that never signed in, which AUTH_REQUIRED=false still
        //allows. The column tightens once that switch is gone
    }
    if (class_id.has_value()) {
        sqlite3_bind_int64(stmt, 2, *class_id);
    } else {
        sqlite3_bind_null(stmt, 2);
        //private practice: no class, so no teacher can see it
    }
    sqlite3_bind_text(stmt, 3, language_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, gemini_key_name.c_str(), -1, SQLITE_TRANSIENT);

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

    if (sqlite3_changes(db_) > 0) {
        close_required_questions(attempt_id);
        //here rather than at each caller, so no way of ending an exam - the
        //clock, the End button, a dropped socket - can forget to settle them
    }
}

// ---- classes ---------------------------------------------------------------

namespace {

// Owns one prepared statement for the length of a scope. The methods above
// finalize by hand; the class methods below take several steps each, and a
// throw between prepare and finalize is exactly where a hand-written finalize
// gets skipped.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("prepare: ") +
                                     sqlite3_errmsg(db) + " in: " + sql);
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& int64(int index, std::int64_t value) {
        sqlite3_bind_int64(stmt_, index, value);
        return *this;
    }
    Statement& text(int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
        return *this;
    }

    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("step: ") + sqlite3_errmsg(db_));
    }
    //true while there is a row to read, false once the statement is done

    void run() {
        while (row()) {
        }
    }

    std::int64_t col_int64(int col) { return sqlite3_column_int64(stmt_, col); }
    std::string col_text(int col) {
        const unsigned char* value = sqlite3_column_text(stmt_, col);
        return value ? std::string(reinterpret_cast<const char*>(value))
                     : std::string();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

// Eight characters from 31 symbols: about 8.5 * 10^11 codes, far past guessing
// range. 0/O and 1/I/L are left out because the code is read off a board.
constexpr char kJoinAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr std::size_t kJoinAlphabetSize = sizeof(kJoinAlphabet) - 1;
constexpr std::size_t kJoinCodeLength = 8;

std::string random_join_code() {
    unsigned char bytes[kJoinCodeLength];
    if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1) {
        throw std::runtime_error("RAND_bytes failed: no secure randomness");
    }
    std::string code;
    code.reserve(kJoinCodeLength);
    for (const unsigned char byte : bytes) {
        code.push_back(kJoinAlphabet[byte % kJoinAlphabetSize]);
    }
    return code;
    //256 is not a multiple of 31, so the modulo leans very slightly towards the
    //first few symbols. The code is a door handle, not a secret key
}

// What a student typed, as the column stores it: upper case, separators gone.
std::string normalise_join_code(const std::string& typed) {
    std::string code;
    for (const char ch : typed) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            code.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    return code;
}

constexpr const char* kClassColumns =
    "c.id, c.name, c.language_id, COALESCE(c.join_code, ''), c.owner_id, "
    "c.created_at, c.archived_at IS NOT NULL, "
    "(SELECT COUNT(*) FROM class_members s "
    "  WHERE s.class_id = c.id AND s.role = 'student')";
//one list, read by read_class, so the indices cannot drift from the queries

ClassInfo read_class(Statement& stmt) {
    ClassInfo info;
    info.id = stmt.col_int64(0);
    info.name = stmt.col_text(1);
    info.language_id = stmt.col_text(2);
    info.join_code = stmt.col_text(3);
    info.owner_id = stmt.col_int64(4);
    info.created_at = stmt.col_int64(5);
    info.archived = stmt.col_int64(6) != 0;
    info.student_count = static_cast<int>(stmt.col_int64(7));
    return info;
}

constexpr const char* kAttemptColumns =
    "a.id, COALESCE(a.user_id, 0), COALESCE(a.class_id, 0), "
    "COALESCE(u.display_name, ''), COALESCE(u.email, ''), a.language_id, "
    "a.started_at, COALESCE(a.ended_at, 0), COALESCE(a.end_reason, ''), "
    "a.turn_count, a.plan_name";

AttemptSummary read_attempt(Statement& stmt) {
    AttemptSummary attempt;
    attempt.id = stmt.col_int64(0);
    attempt.user_id = stmt.col_int64(1);
    attempt.class_id = stmt.col_int64(2);
    attempt.student_name = stmt.col_text(3);
    attempt.student_email = stmt.col_text(4);
    attempt.language_id = stmt.col_text(5);
    attempt.started_at = stmt.col_int64(6);
    attempt.ended_at = stmt.col_int64(7);
    attempt.end_reason = stmt.col_text(8);
    attempt.turn_count = static_cast<int>(stmt.col_int64(9));
    attempt.plan_name = stmt.col_text(10);
    return attempt;
}

}  // namespace

const char* class_role_name(ClassRole role) {
    return role == ClassRole::Teacher ? "teacher" : "student";
}

std::optional<ClassRole> class_role_from_name(const std::string& name) {
    if (name == "teacher") return ClassRole::Teacher;
    if (name == "student") return ClassRole::Student;
    return std::nullopt;
}

std::string Store::unused_join_code() {
    for (int tries = 0; tries < 8; ++tries) {
        std::string code = random_join_code();
        Statement taken(db_, "SELECT 1 FROM classes WHERE join_code = ?");
        taken.text(1, code);
        if (!taken.row()) {
            return code;
        }
    }
    throw std::runtime_error("could not find an unused join code");
    //eight collisions in a row among 8.5 * 10^11 codes means something other than
    //bad luck is wrong, and looping forever would hide it
}

ClassInfo Store::create_class(std::int64_t owner_id,
                              const std::string& name,
                              const std::string& language_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    exec("BEGIN IMMEDIATE");
    std::int64_t class_id = 0;
    try {
        Statement insert(db_,
            "INSERT INTO classes (name, language_id, join_code, owner_id, "
            " created_at) "
            "VALUES (?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER))");
        insert.text(1, name).text(2, language_id).text(3, unused_join_code())
              .int64(4, owner_id).run();
        class_id = sqlite3_last_insert_rowid(db_);

        Statement member(db_,
            "INSERT INTO class_members (class_id, user_id, role, added_at) "
            "VALUES (?, ?, 'teacher', CAST(strftime('%s','now') AS INTEGER))");
        member.int64(1, class_id).int64(2, owner_id).run();

        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }

    auto created = class_by_id(class_id);
    if (!created) {
        throw std::runtime_error("class vanished immediately after insert");
    }
    created->caller_role = ClassRole::Teacher;
    return *created;
}

std::vector<ClassInfo> Store::classes_for_user(std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kClassColumns + ", m.role "
        "FROM classes c JOIN class_members m ON m.class_id = c.id "
        "WHERE m.user_id = ? "
        "ORDER BY c.archived_at IS NOT NULL, c.name COLLATE NOCASE";
    Statement stmt(db_, sql.c_str());
    stmt.int64(1, user_id);

    std::vector<ClassInfo> classes;
    while (stmt.row()) {
        ClassInfo info = read_class(stmt);
        info.caller_role = class_role_from_name(stmt.col_text(8));
        classes.push_back(std::move(info));
    }
    return classes;
}

std::optional<ClassInfo> Store::class_by_id(std::int64_t class_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kClassColumns + " FROM classes c WHERE c.id = ?";
    Statement stmt(db_, sql.c_str());
    stmt.int64(1, class_id);
    if (!stmt.row()) return std::nullopt;
    return read_class(stmt);
}

std::optional<ClassInfo> Store::class_by_join_code(const std::string& code) {
    const std::string normalised = normalise_join_code(code);
    if (normalised.size() != kJoinCodeLength) return std::nullopt;
    //the wrong length can never match, so it is not worth a query

    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kClassColumns +
        " FROM classes c WHERE c.join_code = ? AND c.archived_at IS NULL";
    Statement stmt(db_, sql.c_str());
    stmt.text(1, normalised);
    if (!stmt.row()) return std::nullopt;
    return read_class(stmt);
}

std::optional<ClassRole> Store::class_role(std::int64_t class_id,
                                           std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT role FROM class_members WHERE class_id = ? AND user_id = ?");
    stmt.int64(1, class_id).int64(2, user_id);
    if (!stmt.row()) return std::nullopt;
    return class_role_from_name(stmt.col_text(0));
}

bool Store::add_member(std::int64_t class_id, std::int64_t user_id,
                       ClassRole role) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "INSERT OR IGNORE INTO class_members (class_id, user_id, role, added_at) "
        "VALUES (?, ?, ?, CAST(strftime('%s','now') AS INTEGER))");
    stmt.int64(1, class_id).int64(2, user_id).text(3, class_role_name(role)).run();
    return sqlite3_changes(db_) > 0;
    //OR IGNORE on the (class_id, user_id) key: a second join is a no-op rather
    //than an error, and never rewrites the role already held
}

bool Store::remove_member(std::int64_t class_id, std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "DELETE FROM class_members WHERE class_id = ? AND user_id = ?");
    stmt.int64(1, class_id).int64(2, user_id).run();
    return sqlite3_changes(db_) > 0;
    //the student's attempts keep their class_id, but class_attempts only lists
    //current members, so a removed student's history leaves the teacher's view
}

std::vector<ClassMember> Store::class_members(std::int64_t class_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT u.id, u.email, u.display_name, m.role, u.year_level, "
        "       u.subject_level, m.added_at, "
        "       (SELECT COUNT(*) FROM exam_attempts a "
        "         WHERE a.class_id = m.class_id AND a.user_id = u.id), "
        "       (SELECT COALESCE(MAX(a.started_at), 0) FROM exam_attempts a "
        "         WHERE a.class_id = m.class_id AND a.user_id = u.id) "
        "FROM class_members m JOIN users u ON u.id = m.user_id "
        "WHERE m.class_id = ? "
        "ORDER BY m.role = 'student', u.display_name COLLATE NOCASE, u.email");
    //teachers first: role = 'student' is 0 for them
    stmt.int64(1, class_id);

    std::vector<ClassMember> members;
    while (stmt.row()) {
        ClassMember member;
        member.user_id = stmt.col_int64(0);
        member.email = stmt.col_text(1);
        member.display_name = stmt.col_text(2);
        member.role =
            class_role_from_name(stmt.col_text(3)).value_or(ClassRole::Student);
        member.year_level = stmt.col_text(4);
        member.subject_level = stmt.col_text(5);
        member.added_at = stmt.col_int64(6);
        member.attempt_count = static_cast<int>(stmt.col_int64(7));
        member.last_attempt_at = stmt.col_int64(8);
        members.push_back(std::move(member));
    }
    return members;
}

std::string Store::rotate_join_code(std::int64_t class_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string code = unused_join_code();
    Statement stmt(db_, "UPDATE classes SET join_code = ? WHERE id = ?");
    stmt.text(1, code).int64(2, class_id).run();
    return code;
}

void Store::disable_join_code(std::int64_t class_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_, "UPDATE classes SET join_code = NULL WHERE id = ?");
    stmt.int64(1, class_id).run();
    //NULL rather than '': the column is UNIQUE, and SQLite lets any number of
    //rows hold NULL where only one could hold the empty string
}

void Store::set_archived(std::int64_t class_id, bool archived) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        archived
            ? "UPDATE classes SET archived_at = COALESCE(archived_at, "
              "  CAST(strftime('%s','now') AS INTEGER)) WHERE id = ?"
            : "UPDATE classes SET archived_at = NULL WHERE id = ?");
    stmt.int64(1, class_id).run();
}

InviteResult Store::invite_emails(std::int64_t class_id,
                                  const std::vector<std::string>& emails,
                                  std::int64_t invited_by) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    InviteResult result;
    exec("BEGIN IMMEDIATE");
    try {
        for (const std::string& email : emails) {
            Statement find(db_, "SELECT id FROM users WHERE email = ?");
            find.text(1, email);

            if (find.row()) {
                const std::int64_t user_id = find.col_int64(0);
                Statement member(db_,
                    "INSERT OR IGNORE INTO class_members "
                    "(class_id, user_id, role, added_at) "
                    "VALUES (?, ?, 'student', "
                    "        CAST(strftime('%s','now') AS INTEGER))");
                member.int64(1, class_id).int64(2, user_id).run();
                (sqlite3_changes(db_) > 0 ? result.added : result.existing)++;
                continue;
                //an account already exists, so there is nobody to wait for.
                //users.email is COLLATE NOCASE, so the capitals a teacher typed
                //still find the row
            }

            Statement invite(db_,
                "INSERT OR IGNORE INTO class_invites "
                "(class_id, email, role, invited_by, created_at) "
                "VALUES (?, ?, 'student', ?, "
                "        CAST(strftime('%s','now') AS INTEGER))");
            invite.int64(1, class_id).text(2, email).int64(3, invited_by).run();
            (sqlite3_changes(db_) > 0 ? result.invited : result.existing)++;
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
    return result;
    //one transaction for the whole paste, so a list that fails half way leaves
    //no half-invited class behind it
}

std::vector<ClassInvite> Store::pending_invites(std::int64_t class_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT id, email, created_at FROM class_invites "
        "WHERE class_id = ? AND claimed_at IS NULL ORDER BY email");
    stmt.int64(1, class_id);

    std::vector<ClassInvite> invites;
    while (stmt.row()) {
        invites.push_back(ClassInvite{stmt.col_int64(0), stmt.col_text(1),
                                      stmt.col_int64(2)});
    }
    return invites;
}

bool Store::revoke_invite(std::int64_t class_id, std::int64_t invite_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "DELETE FROM class_invites "
        "WHERE id = ? AND class_id = ? AND claimed_at IS NULL");
    stmt.int64(1, invite_id).int64(2, class_id).run();
    return sqlite3_changes(db_) > 0;
    //class_id in the WHERE as well as the id, so a teacher of one class cannot
    //revoke another class's invite by guessing its number
}

int Store::claim_invites(const User& user) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    int joined = 0;
    exec("BEGIN IMMEDIATE");
    try {
        Statement open(db_,
            "SELECT i.id, i.class_id, i.role FROM class_invites i "
            "JOIN classes c ON c.id = i.class_id "
            "WHERE i.email = ? AND i.claimed_at IS NULL "
            "  AND c.archived_at IS NULL");
        open.text(1, user.email);

        struct Pending {
            std::int64_t id;
            std::int64_t class_id;
            std::string role;
        };
        std::vector<Pending> pending;
        while (open.row()) {
            pending.push_back(
                Pending{open.col_int64(0), open.col_int64(1), open.col_text(2)});
        }
        //read out in full before writing: inserting while the SELECT is still
        //stepping over the same tables is legal in SQLite but easy to get wrong

        for (const Pending& invite : pending) {
            const ClassRole role =
                class_role_from_name(invite.role).value_or(ClassRole::Student);
            Statement member(db_,
                "INSERT OR IGNORE INTO class_members "
                "(class_id, user_id, role, added_at) "
                "VALUES (?, ?, ?, CAST(strftime('%s','now') AS INTEGER))");
            member.int64(1, invite.class_id).int64(2, user.id)
                  .text(3, class_role_name(role)).run();
            joined += sqlite3_changes(db_) > 0 ? 1 : 0;

            Statement claim(db_,
                "UPDATE class_invites "
                "SET claimed_at = CAST(strftime('%s','now') AS INTEGER) "
                "WHERE id = ?");
            claim.int64(1, invite.id).run();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
    return joined;
}

std::vector<AttemptSummary> Store::class_attempts(std::int64_t class_id,
                                                  int limit) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kAttemptColumns +
        " FROM exam_attempts a LEFT JOIN users u ON u.id = a.user_id "
        "WHERE a.class_id = ? "
        "  AND a.user_id IN (SELECT user_id FROM class_members "
        "                    WHERE class_id = a.class_id) "
        "ORDER BY a.started_at DESC, a.id DESC LIMIT ?";
    //current members only: removing a student from the class is also what
    //takes their exams out of the teacher's view
    Statement stmt(db_, sql.c_str());
    stmt.int64(1, class_id).int64(2, limit);

    std::vector<AttemptSummary> attempts;
    while (stmt.row()) {
        attempts.push_back(read_attempt(stmt));
    }
    return attempts;
}

std::vector<AttemptSummary> Store::user_attempts(std::int64_t user_id,
                                                 int limit) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kAttemptColumns +
        " FROM exam_attempts a LEFT JOIN users u ON u.id = a.user_id "
        "WHERE a.user_id = ? "
        "ORDER BY a.started_at DESC, a.id DESC LIMIT ?";
    //no membership clause: these are the caller's own attempts, so leaving a
    //class hides them from that teacher but never from the student who sat them
    Statement stmt(db_, sql.c_str());
    stmt.int64(1, user_id).int64(2, limit);

    std::vector<AttemptSummary> attempts;
    while (stmt.row()) {
        attempts.push_back(read_attempt(stmt));
    }
    return attempts;
}

std::optional<AttemptSummary> Store::attempt_by_id(std::int64_t attempt_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kAttemptColumns +
        " FROM exam_attempts a LEFT JOIN users u ON u.id = a.user_id "
        "WHERE a.id = ?";
    Statement stmt(db_, sql.c_str());
    stmt.int64(1, attempt_id);
    if (!stmt.row()) return std::nullopt;
    return read_attempt(stmt);
}

std::vector<AttemptTurn> Store::attempt_turns(std::int64_t attempt_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT turn_index, role, text, topic, created_at FROM attempt_turns "
        "WHERE attempt_id = ? ORDER BY turn_index");
    stmt.int64(1, attempt_id);

    std::vector<AttemptTurn> turns;
    while (stmt.row()) {
        AttemptTurn turn;
        turn.turn_index = static_cast<int>(stmt.col_int64(0));
        turn.role = stmt.col_text(1);
        turn.text = stmt.col_text(2);
        turn.topic = stmt.col_text(3);
        turn.created_at = stmt.col_int64(4);
        turns.push_back(std::move(turn));
    }
    return turns;
}

// ---- exam plans ------------------------------------------------------------

namespace {

constexpr const char* kPlanColumns =
    "p.id, p.class_id, p.name, COALESCE(p.duration_seconds, 0), "
    "p.require_opinion, p.paraphrase_ok, p.visible, p.archived_at IS NOT NULL, "
    "COALESCE(c.default_plan_id = p.id, 0), p.updated_at";

ExamPlan read_plan(Statement& stmt) {
    ExamPlan plan;
    plan.id = stmt.col_int64(0);
    plan.class_id = stmt.col_int64(1);
    plan.name = stmt.col_text(2);
    plan.duration_seconds = static_cast<int>(stmt.col_int64(3));
    plan.require_opinion = stmt.col_int64(4) != 0;
    plan.paraphrase_ok = stmt.col_int64(5) != 0;
    plan.visible = stmt.col_int64(6) != 0;
    plan.archived = stmt.col_int64(7) != 0;
    plan.is_default = stmt.col_int64(8) != 0;
    plan.updated_at = stmt.col_int64(9);
    return plan;
}

// The three child lists, read after the plan row itself. Three small queries
// rather than one join, because a join of three one-to-many lists multiplies
// the rows out and has to be folded back together anyway.
void load_plan_children(sqlite3* db, ExamPlan& plan) {
    Statement topics(db,
        "SELECT topic_group FROM exam_plan_topics "
        "WHERE plan_id = ? ORDER BY position");
    topics.int64(1, plan.id);
    while (topics.row()) {
        plan.topics.push_back(topics.col_text(0));
    }

    Statement questions(db,
        "SELECT id, text, topic_group, placement FROM exam_plan_questions "
        "WHERE plan_id = ? ORDER BY position");
    questions.int64(1, plan.id);
    while (questions.row()) {
        plan.questions.push_back(PlanQuestion{questions.col_int64(0),
                                              questions.col_text(1),
                                              questions.col_text(2),
                                              questions.col_text(3)});
    }

    Statement tenses(db,
        "SELECT tense, min_count FROM exam_plan_tenses "
        "WHERE plan_id = ? ORDER BY tense");
    tenses.int64(1, plan.id);
    while (tenses.row()) {
        plan.tenses.push_back(TenseTarget{
            tenses.col_text(0), static_cast<int>(tenses.col_int64(1))});
    }
}

}  // namespace

std::vector<ExamPlan> Store::class_plans(std::int64_t class_id,
                                         bool include_archived) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kPlanColumns +
        " FROM exam_plans p JOIN classes c ON c.id = p.class_id "
        "WHERE p.class_id = ?" +
        (include_archived ? "" : " AND p.archived_at IS NULL") +
        " ORDER BY p.archived_at IS NOT NULL, p.name COLLATE NOCASE";
    Statement stmt(db_, sql.c_str());
    stmt.int64(1, class_id);

    std::vector<ExamPlan> plans;
    while (stmt.row()) {
        plans.push_back(read_plan(stmt));
    }
    for (ExamPlan& plan : plans) {
        load_plan_children(db_, plan);
    }
    return plans;
}

std::optional<ExamPlan> Store::plan_by_id(std::int64_t plan_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    const std::string sql =
        std::string("SELECT ") + kPlanColumns +
        " FROM exam_plans p JOIN classes c ON c.id = p.class_id WHERE p.id = ?";
    Statement stmt(db_, sql.c_str());
    stmt.int64(1, plan_id);
    if (!stmt.row()) return std::nullopt;

    ExamPlan plan = read_plan(stmt);
    load_plan_children(db_, plan);
    return plan;
}

ExamPlan Store::save_plan(const ExamPlan& plan, std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    std::int64_t plan_id = plan.id;
    exec("BEGIN IMMEDIATE");
    try {
        if (plan_id == 0) {
            Statement insert(db_,
                "INSERT INTO exam_plans (class_id, name, duration_seconds, "
                " require_opinion, paraphrase_ok, visible, created_by, "
                " created_at, updated_at) "
                "VALUES (?, ?, NULLIF(?, 0), ?, ?, ?, ?, "
                "        CAST(strftime('%s','now') AS INTEGER), "
                "        CAST(strftime('%s','now') AS INTEGER))");
            insert.int64(1, plan.class_id).text(2, plan.name)
                  .int64(3, plan.duration_seconds)
                  .int64(4, plan.require_opinion ? 1 : 0)
                  .int64(5, plan.paraphrase_ok ? 1 : 0)
                  .int64(6, plan.visible ? 1 : 0)
                  .int64(7, user_id).run();
            plan_id = sqlite3_last_insert_rowid(db_);
        } else {
            Statement update(db_,
                "UPDATE exam_plans SET name = ?, "
                " duration_seconds = NULLIF(?, 0), require_opinion = ?, "
                " paraphrase_ok = ?, visible = ?, "
                " updated_at = CAST(strftime('%s','now') AS INTEGER) "
                "WHERE id = ? AND class_id = ?");
            update.text(1, plan.name).int64(2, plan.duration_seconds)
                  .int64(3, plan.require_opinion ? 1 : 0)
                  .int64(4, plan.paraphrase_ok ? 1 : 0)
                  .int64(5, plan.visible ? 1 : 0)
                  .int64(6, plan_id).int64(7, plan.class_id).run();
            if (sqlite3_changes(db_) == 0) {
                throw std::runtime_error("save_plan: no such plan in that class");
            }
            //class_id in the WHERE: the caller checked the teacher owns the
            //class, and this makes sure the plan is that class's too

            for (const char* table : {"exam_plan_topics", "exam_plan_questions",
                                      "exam_plan_tenses"}) {
                const std::string sql =
                    std::string("DELETE FROM ") + table + " WHERE plan_id = ?";
                Statement clear(db_, sql.c_str());
                clear.int64(1, plan_id).run();
            }
        }

        int position = 0;
        for (const std::string& topic : plan.topics) {
            Statement row(db_,
                "INSERT OR IGNORE INTO exam_plan_topics "
                "(plan_id, topic_group, position) VALUES (?, ?, ?)");
            row.int64(1, plan_id).text(2, topic).int64(3, position++).run();
        }

        position = 0;
        for (const PlanQuestion& question : plan.questions) {
            Statement row(db_,
                "INSERT INTO exam_plan_questions "
                "(plan_id, text, topic_group, placement, position) "
                "VALUES (?, ?, ?, ?, ?)");
            row.int64(1, plan_id).text(2, question.text)
               .text(3, question.topic_group).text(4, question.placement)
               .int64(5, position++).run();
        }

        for (const TenseTarget& target : plan.tenses) {
            Statement row(db_,
                "INSERT OR REPLACE INTO exam_plan_tenses (plan_id, tense, min_count) "
                "VALUES (?, ?, ?)");
            row.int64(1, plan_id).text(2, target.tense)
               .int64(3, target.min_count).run();
        }

        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }

    auto saved = plan_by_id(plan_id);
    if (!saved) {
        throw std::runtime_error("plan vanished immediately after save");
    }
    return *saved;
}

void Store::set_default_plan(std::int64_t class_id,
                             std::optional<std::int64_t> plan_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    if (!plan_id) {
        Statement clear(db_,
            "UPDATE classes SET default_plan_id = NULL WHERE id = ?");
        clear.int64(1, class_id).run();
        return;
    }
    Statement set(db_,
        "UPDATE classes SET default_plan_id = ? WHERE id = ? AND EXISTS "
        "(SELECT 1 FROM exam_plans WHERE id = ? AND class_id = ? "
        "   AND archived_at IS NULL)");
    set.int64(1, *plan_id).int64(2, class_id).int64(3, *plan_id)
       .int64(4, class_id).run();
    if (sqlite3_changes(db_) == 0) {
        throw std::runtime_error("set_default_plan: plan is not a live plan of that class");
    }
}

void Store::archive_plan(std::int64_t plan_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    exec("BEGIN IMMEDIATE");
    try {
        Statement archive(db_,
            "UPDATE exam_plans SET archived_at = COALESCE(archived_at, "
            "  CAST(strftime('%s','now') AS INTEGER)) WHERE id = ?");
        archive.int64(1, plan_id).run();

        Statement unset(db_,
            "UPDATE classes SET default_plan_id = NULL WHERE default_plan_id = ?");
        unset.int64(1, plan_id).run();
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

void Store::attach_plan(std::int64_t attempt_id, const ExamPlan& plan,
                        const std::string& plan_json) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    exec("BEGIN IMMEDIATE");
    try {
        Statement attempt(db_,
            "UPDATE exam_attempts SET plan_id = ?, plan_name = ?, plan_json = ? "
            "WHERE id = ?");
        attempt.int64(1, plan.id).text(2, plan.name).text(3, plan_json)
               .int64(4, attempt_id).run();

        for (const PlanQuestion& question : plan.questions) {
            Statement row(db_,
                "INSERT OR IGNORE INTO attempt_required_questions "
                "(attempt_id, question_id, text, topic_group) VALUES (?, ?, ?, ?)");
            row.int64(1, attempt_id).int64(2, question.id).text(3, question.text)
               .text(4, question.topic_group).run();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

void Store::record_turn_features(std::int64_t attempt_id, int turn_index,
                                 const std::string& kind,
                                 const std::vector<std::string>& values,
                                 const std::string& source) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    for (const std::string& value : values) {
        Statement row(db_,
            "INSERT OR IGNORE INTO turn_features "
            "(attempt_id, turn_index, kind, value, source) VALUES (?, ?, ?, ?, ?)");
        row.int64(1, attempt_id).int64(2, turn_index).text(3, kind)
           .text(4, value).text(5, source).run();
    }
}

void Store::mark_required_question(std::int64_t attempt_id,
                                   std::int64_t question_id,
                                   const std::string& status,
                                   int turn_index) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement row(db_,
        "UPDATE attempt_required_questions SET status = ?, turn_index = ? "
        "WHERE attempt_id = ? AND question_id = ?");
    row.text(1, status).int64(2, turn_index).int64(3, attempt_id)
       .int64(4, question_id).run();
}

void Store::close_required_questions(std::int64_t attempt_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement row(db_,
        "UPDATE attempt_required_questions SET status = 'missed' "
        "WHERE attempt_id = ? AND status = 'pending'");
    row.int64(1, attempt_id).run();
}

std::vector<TurnFeature> Store::attempt_features(std::int64_t attempt_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT turn_index, kind, value, source FROM turn_features "
        "WHERE attempt_id = ? ORDER BY turn_index, kind, value, source");
    stmt.int64(1, attempt_id);

    std::vector<TurnFeature> features;
    while (stmt.row()) {
        features.push_back(TurnFeature{static_cast<int>(stmt.col_int64(0)),
                                       stmt.col_text(1), stmt.col_text(2),
                                       stmt.col_text(3)});
    }
    return features;
}

std::vector<RequiredQuestionStatus> Store::attempt_required(std::int64_t attempt_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT question_id, text, topic_group, status, COALESCE(turn_index, -1) "
        "FROM attempt_required_questions WHERE attempt_id = ? "
        "ORDER BY question_id");
    stmt.int64(1, attempt_id);

    std::vector<RequiredQuestionStatus> rows;
    while (stmt.row()) {
        rows.push_back(RequiredQuestionStatus{
            stmt.col_int64(0), stmt.col_text(1), stmt.col_text(2),
            stmt.col_text(3), static_cast<int>(stmt.col_int64(4))});
    }
    return rows;
}

std::vector<CoverageRow> Store::class_coverage(std::int64_t class_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT a.user_id, t.role, f.kind, f.value, "
        "       COUNT(DISTINCT f.attempt_id || ':' || f.turn_index) "
        "FROM turn_features f "
        "JOIN exam_attempts a ON a.id = f.attempt_id "
        "JOIN attempt_turns t ON t.attempt_id = f.attempt_id "
        "                    AND t.turn_index = f.turn_index "
        "WHERE a.class_id = ?1 "
        "  AND a.user_id IN (SELECT user_id FROM class_members WHERE class_id = ?1) "
        "GROUP BY a.user_id, t.role, f.kind, f.value "
        "UNION ALL "
        "SELECT a.user_id, 'examiner', 'topic', t.topic, COUNT(*) "
        "FROM attempt_turns t JOIN exam_attempts a ON a.id = t.attempt_id "
        "WHERE a.class_id = ?1 AND t.role = 'examiner' AND t.topic != '' "
        "  AND a.user_id IN (SELECT user_id FROM class_members WHERE class_id = ?1) "
        "GROUP BY a.user_id, t.topic");
    //a turn counts once per tense however many sources agreed on it: the
    //report asks "was it used", and the two sources are shown apart elsewhere.
    //Current members only, the same rule class_attempts lists by
    stmt.int64(1, class_id);

    std::vector<CoverageRow> rows;
    while (stmt.row()) {
        rows.push_back(CoverageRow{stmt.col_int64(0), stmt.col_text(1),
                                   stmt.col_text(2), stmt.col_text(3),
                                   static_cast<int>(stmt.col_int64(4))});
    }
    return rows;
}

// ---- paid access and usage -------------------------------------------------

PaidAccess Store::paid_access(std::int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT l.kind, l.ends_at FROM licences l "
        "WHERE l.revoked_at IS NULL "
        "  AND l.starts_at <= CAST(strftime('%s','now') AS INTEGER) "
        "  AND l.ends_at   >  CAST(strftime('%s','now') AS INTEGER) "
        "  AND ((l.kind = 'user' AND l.target_id = ?1) "
        "    OR (l.kind = 'class' AND l.target_id IN "
        "        (SELECT m.class_id FROM class_members m "
        "         JOIN classes c ON c.id = m.class_id "
        "         WHERE m.user_id = ?1 AND c.archived_at IS NULL))) "
        "ORDER BY l.ends_at DESC LIMIT 1");
    //a class licence covers whoever is in the class today: joining gives it,
    //leaving or the class being archived takes it away, with nothing to sync
    stmt.int64(1, user_id);

    PaidAccess access;
    if (stmt.row()) {
        access.active = true;
        access.source = stmt.col_text(0);
        access.until = stmt.col_int64(1);
    }
    return access;
}

std::int64_t Store::grant_licence(const std::string& kind, std::int64_t target_id,
                                  const std::string& until_date,
                                  const std::string& note) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "INSERT INTO licences (kind, target_id, starts_at, ends_at, note, created_at) "
        "VALUES (?, ?, CAST(strftime('%s','now') AS INTEGER), "
        "        CAST(strftime('%s', ? || ' 23:59:59', 'utc') AS INTEGER), ?, "
        "        CAST(strftime('%s','now') AS INTEGER))");
    //'utc' reads the date as local time and converts it, so the licence ends
    //at the end of that day where the server is, not in Greenwich
    stmt.text(1, kind).int64(2, target_id).text(3, until_date).text(4, note).run();
    return sqlite3_last_insert_rowid(db_);
}

bool Store::revoke_licence(std::int64_t licence_id) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "UPDATE licences SET revoked_at = CAST(strftime('%s','now') AS INTEGER) "
        "WHERE id = ? AND revoked_at IS NULL");
    stmt.int64(1, licence_id).run();
    return sqlite3_changes(db_) > 0;
}

std::vector<Licence> Store::licences() {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT l.id, l.kind, l.target_id, "
        "       COALESCE(CASE l.kind WHEN 'user' THEN u.email ELSE c.name END, '?'), "
        "       l.starts_at, l.ends_at, l.note, l.revoked_at IS NOT NULL "
        "FROM licences l "
        "LEFT JOIN users u ON l.kind = 'user' AND u.id = l.target_id "
        "LEFT JOIN classes c ON l.kind = 'class' AND c.id = l.target_id "
        "ORDER BY l.ends_at DESC");

    std::vector<Licence> rows;
    while (stmt.row()) {
        Licence licence;
        licence.id = stmt.col_int64(0);
        licence.kind = stmt.col_text(1);
        licence.target_id = stmt.col_int64(2);
        licence.target_label = stmt.col_text(3);
        licence.starts_at = stmt.col_int64(4);
        licence.ends_at = stmt.col_int64(5);
        licence.note = stmt.col_text(6);
        licence.revoked = stmt.col_int64(7) != 0;
        rows.push_back(std::move(licence));
    }
    return rows;
}

std::optional<User> Store::user_by_email(const std::string& email) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_, "SELECT id FROM users WHERE email = ?");
    stmt.text(1, email);
    if (!stmt.row()) return std::nullopt;
    return user_by_id(stmt.col_int64(0));
}

int Store::usage_today(std::int64_t user_id, const std::string& feature) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "SELECT used FROM usage_daily "
        "WHERE user_id = ? AND day = date('now','localtime') AND feature = ?");
    stmt.int64(1, user_id).text(2, feature);
    return stmt.row() ? static_cast<int>(stmt.col_int64(0)) : 0;
}

std::optional<int> Store::reserve_usage(std::int64_t user_id,
                                        const std::string& feature, int limit) {
    if (limit <= 0) return std::nullopt;

    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "INSERT INTO usage_daily (user_id, day, feature, used) "
        "VALUES (?1, date('now','localtime'), ?2, 1) "
        "ON CONFLICT (user_id, day, feature) DO UPDATE SET used = used + 1 "
        "WHERE used < ?3");
    //the WHERE on the upsert is the whole check: at the limit the update does
    //nothing, sqlite3_changes reports 0, and no unit was spent
    stmt.int64(1, user_id).text(2, feature).int64(3, limit).run();
    if (sqlite3_changes(db_) == 0) {
        return std::nullopt;
    }
    return usage_today(user_id, feature);
}

void Store::release_usage(std::int64_t user_id, const std::string& feature) {
    std::lock_guard<std::recursive_mutex> lock(m_);

    Statement stmt(db_,
        "UPDATE usage_daily SET used = MAX(used - 1, 0) "
        "WHERE user_id = ? AND day = date('now','localtime') AND feature = ?");
    stmt.int64(1, user_id).text(2, feature).run();
}

}  // namespace sim
