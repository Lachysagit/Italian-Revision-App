#include "sim/store.hpp"

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

    if (version >= 1) {
        return;
    }

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

}  // namespace sim
