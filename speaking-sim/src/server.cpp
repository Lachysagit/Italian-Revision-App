#include "sim/server.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sim/audio_encode.hpp"
#include "sim/http_util.hpp"
#include "sim/plan_json.hpp"
#include "sim/tenses.hpp"
#include "sim/protocol.hpp"
#include "sim/question_bank.hpp"
#include "sim/static_files.hpp"
#include "sim/text_clean.hpp"
#include "sim/topics.hpp"
#include "sim/translate.hpp"
#include "sim/session.hpp"

namespace sim {

namespace {

// Both are escape hatches, not schedules: the barrier fills in microseconds and
// the handshakes take a few hundred milliseconds. They exist so that a pool
// smaller than worker_threads, or a network that never answers, costs a slow
// first turn instead of a server that never finishes starting.
constexpr int kPrewarmBarrierSeconds = 5;
constexpr int kPrewarmJoinSeconds = 15;

// The translate box is for a sentence at a time. The cap stops an accidental
// paste of the whole transcript turning into one long, billable request.
constexpr std::size_t kMaxTranslateChars = 1000;

// The route only ever serves this app's two directions, so the language pair is
// checked against a list rather than passed through. Without this the request
// body could steer the outbound call at any language Google offers.
bool is_supported_language(const std::string& code) {
    return code == "it" || code == "en" || code == "de";
    //de was missing while the listening page already offered German, so its
    //translate box answered every lookup with an unsupported-pair error
}

constexpr const char* kSessionCookie = "sid";

// The browser starts its countdown when the opening question arrives, a moment
// after the server sent it. Without this slack an answer finished just inside
// the student's five minutes could land just outside the server's.
constexpr int kClockSlackSeconds = 10;

// The usage_daily feature an examiner question is counted under.
constexpr const char* kSpeakingFeature = "speaking_question";

std::string quota_message(int limit) {
    return "You have used all " + std::to_string(limit) +
           " of today's speaking questions, so the exam ends here. Your answer "
           "has been saved, and more questions are available from midnight.";
}

// Who the websocket handshake found behind the session cookie. Allocated in
// onaccept, which runs before the connection object exists, and handed across
// through the connection's userdata pointer; onopen takes it straight back into
// a unique_ptr. Zero is a browser that never signed in.
struct SocketIdentity {
    std::int64_t user_id = 0;
};

//TEACHER_EMAILS is a comma separated allowlist. A school deployment needs some
//way to say who may create a class, and an env var needs no admin UI to go
//with it. Case insensitive, because it is typed by a person.
bool is_teacher_email(const Config& config, const std::string& email) {
    if (config.teacher_emails.empty() || email.empty()) return false;

    std::string lowered;
    lowered.reserve(email.size());
    for (char c : email) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return std::find(config.teacher_emails.begin(), config.teacher_emails.end(),
                     lowered) != config.teacher_emails.end();
}

//the cohort a student may claim. Closed lists: subject_level picks which
//Gemini key pool an exam spends, so it must not be free text from a browser.
bool is_valid_subject_level(const std::string& level) {
    return level == "beginners" || level == "continuers" ||
           level == "advanced" || level == "extension";
}

bool is_valid_year_level(const std::string& year) {
    return year == "7" || year == "8" || year == "9" || year == "10" ||
           year == "11" || year == "12";
}

crow::response auth_error_page(const std::string& message) {
    crow::response response(400,
        "<!doctype html><meta charset=utf-8>"
        "<title>Sign-in failed</title>"
        "<body style=\"font-family:system-ui;max-width:32rem;margin:4rem auto\">"
        "<h1>Sign-in failed</h1><p>" + message + "</p>"
        "<p><a href=\"/auth/login\">Try again</a></p>");
    response.set_header("Content-Type", "text/html; charset=utf-8");
    return response;
    //a page rather than JSON: this is reached by a browser redirect from
    //Google, so whoever hits it is looking at a window, not reading a fetch
}

template <typename F>
void persist_quietly(const char* what, F&& write) {
    try {
        write();
    } catch (const std::exception& e) {
        std::cerr << "persist " << what << " failed, turn continues: "
                  << e.what() << '\n';
    } catch (...) {
        std::cerr << "persist " << what << " failed with a non-std exception, "
                     "turn continues\n";
    }
}
//the exam is the product and the record is bookkeeping, so a database that
//cannot be written must never cost a student their turn. The same split the
//examiner's own failures use: the detail goes to the operator's log and
//nothing about it reaches the student

}  // namespace

Server::Server(Config config,
               std::unique_ptr<InterfaceSTT> stt,
               std::unique_ptr<InterfaceExaminer> examiner,
               std::unique_ptr<InterfaceTTS> tts,
               std::unique_ptr<Store> store,
               std::unique_ptr<SafetyChain> safety)
    : config_(std::move(config)),
      stt_(std::move(stt)),
      examiner_(std::move(examiner)),
      tts_(std::move(tts)),
      safety_(std::move(safety)),
      store_(std::move(store)),
      pool_(config_.worker_threads) {
    //constructor where config_ is initialised
} // constructor

Server::~Server() {
    {
        std::lock_guard<std::mutex> lock(retention_m_);
        retention_stopping_ = true;
    }
    retention_wake_.notify_all();
    if (retention_thread_.joinable()) {
        retention_thread_.join();
    }
    //joined before anything else is destroyed: the thread holds store_ and a
    //detached one outliving this object would purge through a dangling pointer
}

void Server::retention_loop() {
    for (;;) {
        try {
            const PurgeCounts went = store_->purge_expired(config_.retention);
            if (went.total() > 0) {
                std::cerr << "retention: removed " << went.transcripts
                          << " transcript turn(s), " << went.attempts
                          << " attempt(s), " << went.safety_events
                          << " safety event(s), " << went.auth_sessions
                          << " expired session(s), " << went.usage_rows
                          << " usage row(s), " << went.accounts
                          << " account(s)\n";
            }
            //silent when it found nothing, which is the normal day. A line per
            //pass would bury the one that matters under a year of "0 0 0 0"
        } catch (const std::exception& e) {
            std::cerr << "retention: pass failed, will retry tomorrow: "
                      << e.what() << '\n';
            //never throws out of this thread: a purge that cannot run is a
            //compliance problem to fix, not a reason to take the exam server
            //down in the middle of a lesson
        }

        std::unique_lock<std::mutex> lock(retention_m_);
        retention_wake_.wait_for(lock, std::chrono::hours(24),
                                 [this] { return retention_stopping_; });
        if (retention_stopping_) return;
    }
}


void Server::run() 

    {
    languages_ = LanguageRegistry::load(config_);
    //every language's prompts, bank and voice, read once at startup. Which
    //prompt a turn gets is still Session's decision, because only the Session
    //knows whether a question has been asked yet. load() logs per language and
    //never throws: a missing German prompt file must still leave Italian exams
    //running, the same way a missing prompt file always has

    safety_->prewarm();
    //before the port is bound, and before the examiner prewarm occupies every
    //pool thread. Loading the wordlists here is also what makes ready() below
    //answer from something rather than from a directory nobody has opened

    const bool safety_expected = config_.safety_mode != SafetyMode::Off;
    if (safety_expected && !safety_->ready()) {
        if (config_.auth_required) {
            throw std::runtime_error(
                "the safety chain is not ready and AUTH_REQUIRED is on. An "
                "exam that cannot be screened does not begin: check "
                "SAFETY_WORDLIST_DIR (" + config_.safety_wordlist_dir +
                ") and, in azure mode, that Content Safety answered its health "
                "check.");
        }
        std::cerr << "safety: chain is NOT ready - exams will run unscreened. "
                     "This configuration is refused once AUTH_REQUIRED is on\n";
        //loud rather than fatal while AUTH_REQUIRED is off, which is the
        //development build the HSC work happens in
    }
    if (!safety_expected) {
        std::cerr << "safety: SAFETY_MODE=off - NOTHING is screened. "
                     "Development only; load_config refuses this with "
                     "AUTH_REQUIRED on\n";
    }

    prewarm_tts();
    prewarm_examiner();
    //before the port is bound, so the first student to connect cannot race them

    retention_thread_ = std::thread([this] { retention_loop(); });
    //the first pass runs immediately inside the thread rather than here: a
    //database carrying a year of old transcripts should not hold the port
    //closed while it deletes them, and the records are no more overdue for
    //the few seconds it takes the socket to come up

    CROW_ROUTE(app_, "/") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/index.html", "text/html");
    });

    CROW_ROUTE(app_, "/client.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/client.js", "application/javascript");
    });

    CROW_ROUTE(app_, "/account.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/account.js", "application/javascript");
    });
    //the signed-in-as corner of the nav, shared by both pages

    CROW_ROUTE(app_, "/translate.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/translate.js", "application/javascript");
    });
    //the translate box, shared by both pages

    CROW_ROUTE(app_, "/styles.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/styles.css", "text/css");
    });

    CROW_ROUTE(app_, "/fonts.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/fonts.css", "text/css");
    });
    //the @font-face block, linked by both pages

    CROW_ROUTE(app_, "/fonts/<string>") //HTTP ROUTE -----------------------------------
    ([](const std::string& file) {
        if (!is_safe_font_name(file)) {
            return crow::response(404);
        }
        return serve_static_file("web(frontend)/fonts/" + file, "font/woff2");
    });
    //crow's <string> stops at a /, and is_safe_font_name keeps the rest of the
    //folder from being readable through a name the stylesheet never asks for

    CROW_ROUTE(app_, "/teacher") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/teacher.html", "text/html");
    });
    //served to anyone, like every other page: the gate and the API decide what
    //a visitor sees, and the page itself holds nothing but layout

    CROW_ROUTE(app_, "/classes.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/classes.js", "application/javascript");
    });
    //the class picker and join box on the exam page

    CROW_ROUTE(app_, "/join/<string>") //HTTP ROUTE -----------------------------------
    ([](const std::string& code) {
        std::string safe;
        for (const char ch : code) {
            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-') {
                safe.push_back(ch);
            }
        }
        crow::response response(302);
        response.set_header("Location", safe.empty() ? "/" : "/?join=" + safe);
        return response;
        //a link a teacher can paste anywhere. The exam page does the joining,
        //after sign-in if need be; only letters, digits and hyphens are passed
        //on, so the redirect cannot be bent into anything but this site
    });

    CROW_ROUTE(app_, "/classes") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/classes-page.html", "text/html");
    });
    //the student's side of what /teacher is for a teacher: the classes they are
    //in and the exams they have sat. A page rather than a box on the exam page,
    //so it can be linked to and come back to

    CROW_ROUTE(app_, "/classes-page.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/classes-page.js", "application/javascript");
    });
    //named apart from classes.js, which is the exam page's picker

    CROW_ROUTE(app_, "/classes-page.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/classes-page.css", "text/css");
    });

    CROW_ROUTE(app_, "/teacher.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/teacher.js", "application/javascript");
    });

    CROW_ROUTE(app_, "/teacher.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/teacher.css", "text/css");
    });

    CROW_ROUTE(app_, "/listening") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/listening.html", "text/html");
    });

    CROW_ROUTE(app_, "/listening.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/listening.js", "application/javascript");
    });

    CROW_ROUTE(app_, "/listening.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/listening.css", "text/css");
    });

    CROW_ROUTE(app_, "/listening/<string>/manifest.json") //HTTP ROUTE -----------------------------------
    ([this](const std::string& language) {
        return serve_manifest(language);
    });
    //one clip index per language, re-fetched when the page's picker changes

    CROW_ROUTE(app_, "/listening/<string>/clips/<string>/<string>") //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req, const std::string& language,
            const std::string& year, const std::string& file) {
        return serve_clip(req, language, year, file);
    });
    //crow's <string> stops at a /, so the path cannot be walked upward even
    //before is_safe_clip_name looks at it

    CROW_ROUTE(app_, "/api/gemini-keys") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_gemini_keys();
    });
    //the settings modal fetches this to populate its picker

    CROW_ROUTE(app_, "/api/languages") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_languages();
    });
    //and this to populate the language picker beside it

    CROW_ROUTE(app_, "/signin") //HTTP ROUTE -----------------------------------
    ([] {
        crow::response response(302);
        response.set_header("Location", "/");
        return response;
    });
    //kept as a redirect rather than a page: the gate on the exam page is what
    //asks for a sign-in now, and a separate page carrying the same gate would
    //block itself. Old links and bookmarks still land somewhere sensible

    CROW_ROUTE(app_, "/auth/login") //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req) {
        if (auto refusal = refuse_if_rate_limited(
                "login:" + req.remote_ip_address, 20, 60)) {
            return std::move(*refusal);
        }
        return serve_auth_login();
    });
    //by address, since nobody is signed in yet: each call creates a pending
    //login state, and a loop hitting this would fill that table
    //the sign-in button points straight here rather than at Google, so the
    //client id and the PKCE challenge never have to exist in the page

    CROW_ROUTE(app_, "/auth/callback") //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req) {
        return serve_auth_callback(req);
    });
    //where Google sends the browser back. This exact URL has to be listed as an
    //authorised redirect URI on the OAuth client, matched as a literal string

    CROW_ROUTE(app_, "/auth/logout").methods("POST"_method) //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req) {
        return serve_auth_logout(req);
    });
    //POST so that a link or a prefetch cannot sign somebody out

    CROW_ROUTE(app_, "/api/me") //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req) {
        return serve_me(req);
    });
    //what every page asks to find out whether it is signed in and as whom

    CROW_ROUTE(app_, "/api/me/profile").methods("POST"_method) //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req) {
        return serve_set_profile(req);
    });
    //where sign-up finishes: year, subject level and language. Until this has
    //been posted once the account is not onboarded, and no exam may start

    register_class_routes();
    register_plan_routes();
    //exam plans, their options and the class coverage report, in
    //src/plan_api.cpp
    //classes, join codes, rosters and exam history for the teacher dashboard,
    //all in src/class_api.cpp

    CROW_ROUTE(app_, "/api/translate").methods("POST"_method) //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req) {
        return serve_translate(req);
    });
    //the translate box posts here. Handlers run on crow's own socket threads,
    //so this blocks one for the call and never touches pool_ - a lookup cannot
    //take a worker away from a student who is mid-turn

    CROW_WEBSOCKET_ROUTE(app_, "/ws") //WEBSOCKET ROUTE ----------------------------------
        .onaccept([this](const crow::request& req,
                         std::optional<crow::response>& refusal,
                         void** userdata)
            {
            const std::string origin = req.get_header_value("Origin");
            if (origin.empty() ? config_.auth_required
                               : !origin_allowed(origin,
                                                 req.get_header_value("Host"),
                                                 config_.public_origin)) {
                refusal = crow::response(403, "cross-site websocket refused");
                return;
                //the session cookie rides on a websocket from any page, so
                //without this another site could open an exam in a signed-in
                //student's name and read their transcript back
                //
                //an ABSENT Origin is refused too once AUTH_REQUIRED is on.
                //Every browser sends it on a websocket handshake, so the only
                //callers it turns away are the ones not using a browser - and
                //a check that any client can skip by leaving a header off is
                //not a check. The permissive branch stays for the signed-out
                //dev build, where there is no cookie to ride on in the first
                //place and curl is how the socket gets exercised
            }

            std::int64_t user_id = 0;
            const std::string token =
                cookie_value(req.get_header_value("Cookie"), kSessionCookie);
            if (!token.empty()) {
                try {
                    if (const auto user = store_->user_for_auth_token(token)) {
                        user_id = user->id;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "ws: session lookup failed: " << e.what() << '\n';
                    //treated as signed out: with AUTH_REQUIRED on that refuses
                    //below, and with it off the exam still runs anonymously
                }
            }

            if (user_id == 0 && config_.auth_required) {
                refusal = crow::response(401, "sign in to start an exam");
                return;
            }

            if (!limiter_.allow(user_id != 0 ? "ws:" + std::to_string(user_id)
                                             : "ws:" + req.remote_ip_address,
                                30, std::chrono::seconds(60))) {
                refusal = crow::response(429, "too many exams started, wait a minute");
                return;
                //each exam is a new socket, and a page stuck reconnecting in a
                //loop would otherwise open sessions faster than anyone sits them
            }

            *userdata = new SocketIdentity{user_id};
            //read here, at the handshake, because this is the one moment the
            //socket carries the browser's cookies. Each exam opens a fresh
            //socket, so the answer is never older than the exam it belongs to
            }
        ) //end of .onaccept

        .onopen([this](crow::websocket::connection& conn) //handles when websocket is opened
        
            {
            std::unique_ptr<SocketIdentity> identity(
                static_cast<SocketIdentity*>(conn.userdata()));
            conn.userdata(nullptr);
            //ownership taken back at once, so the identity is freed with this
            //scope and nothing later can mistake the pointer for live data

            auto session = std::make_shared<Session>();
            if (identity) {
                session->set_user_id(identity->user_id);
            }
            session->set_language(&languages_.default_pack());
            //the default until a Start message names one, so a client that
            //never sends a language still runs an exam rather than reaching a
            //null pack on its first turn


            //sessions_ maps crow::websocket::connection* keys to
            //std::shared_ptr<Session> values

            auto handle = std::make_shared<ConnHandle>();
            handle->conn = &conn;
            //one handle per connection instance: a worker still holding the
            //old one sees a nulled conn rather than a new connection

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                //local variable lock of type lock_guard which calls lock on session_mutex
                sessions_[&conn] = session;
                //for this conn key in the map assign its value the sharedptr session
                conn_handles_[&conn] = std::move(handle);
                //both maps written under the one scope so they cannot drift

            }
            } 
        ) //end of .onopen 

        .onmessage([this](crow::websocket::connection& conn, //handles when websocket receives message
                          const std::string& data,
                          bool is_binary)  //flag
                          
            {
            //params are conn, data, and binary flag
            auto session = find_session(&conn);
            // find session for this conn
            if (!session) {
                return;
            //if nullptr was returned it evaluates to false which returns
            }

            if (is_binary) { //boolean check
                handle_audio(session, data);
            //binary check
            } else { //handles false of boolean if binary
                handle_control(conn, session, data);
            }
            //text = JSON control message
            }
        ) // end of  .onmessage


        .onclose([this](crow::websocket::connection& conn,
                        const std::string& reason,
                        uint16_t code) 
         //params are connection, reason for close and
        //code is a numeric WebSocket close code
        
        {
            std::shared_ptr<ConnHandle> handle;
            std::shared_ptr<Session> closing_session;

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                //lock the mutex
                auto it = conn_handles_.find(&conn);
                if (it != conn_handles_.end()) {
                    handle = std::move(it->second);
                    conn_handles_.erase(it);
                }
                //lifted out before the erase so the handle survives the map entry
                auto session_it = sessions_.find(&conn);
                if (session_it != sessions_.end()) {
                    closing_session = std::move(session_it->second);
                    sessions_.erase(session_it);
                }
                //lifted out the same way the handle is: the attempt has to be
                //closed below, and that must not happen under the map mutex
            } //mutex is unlocked as the lock variable goes out of scope

            if (closing_session && closing_session->attempt_id() != 0) {
                persist_quietly("attempt end (disconnect)", [&] {
                    store_->end_attempt(closing_session->attempt_id(),
                                        "disconnect");
                });
            }
            //end_attempt only touches rows where ended_at IS NULL, so a socket
            //closing after the timer already ended the exam cannot relabel a
            //clean finish as a disconnect. A turn still in flight on a worker
            //may insert after this, which is harmless: the row is already closed

            //the two scopes are sequential and never nested, deliberately.
            //Holding one while reaching for the other is exactly the deadlock

            CROW_LOG_DEBUG << "websocket closed, code " << code
                           << ", reason: " << reason;
            //reason and code were named and never read. Logging both tells a
            //silent disconnect apart from a client that simply went away

            if (handle) {
                std::lock_guard<std::mutex> lock(handle->m);
                handle->conn = nullptr;
                //blocks until any send in flight releases m; ~Connection
                //cannot begin until this returns, so the send is never cut off
            }
        }
    ); // end of .onclose

    app_.port(config_.port).multithreaded().run(); //IMPORTANT LINE
    //Launches server loop, accepts websocket requests accross multi threads
}

void Server::prewarm_tts() {
    //PiperTTS spawns a fresh piper process per turn, and the first spawn of the
    //run pays for loading the executable, onnxruntime and the voice .onnx off
    //cold disk - seconds, all of it landing on the opening question. Doing it
    //here pulls those pages into the OS file cache while nobody is waiting, so
    //every later spawn is a warm one. The samples are thrown away.
    std::vector<std::string> warmed;
    //each distinct voice once: two languages sharing a voice share its cache
    //entry too, and warming it twice only spends a second spawn on nothing

    for (const LanguagePack* pack : languages_.all()) {
        if (std::find(warmed.begin(), warmed.end(), pack->piper_voice_path) !=
            warmed.end()) {
            continue;
        }
        warmed.push_back(pack->piper_voice_path);

        const auto started = std::chrono::steady_clock::now();
        try {
            const std::vector<std::int16_t> warm =
                tts_->synthesize(pack->prewarm_text, pack->piper_voice_path);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started).count();
            if (warm.empty()) {
                std::cerr << "tts prewarm (" << pack->id << ") produced no audio in "
                          << ms << "ms - piper is not configured, replies will be silent"
                          << std::endl;
            } else {
                std::cerr << "tts prewarm (" << pack->id << "): " << ms << "ms, "
                          << (warm.size() / static_cast<double>(
                                  tts_->sample_rate(pack->piper_voice_path)))
                          << "s of audio discarded" << std::endl;
                //the number to compare against the first turn's tts timing: if they
                //are still close, the cost is synthesis itself, not the cold start
            }
        } catch (const std::exception& e) {
            std::cerr << "tts prewarm (" << pack->id
                      << ") failed, continuing: " << e.what() << std::endl;
            //never fatal, and per voice: a missing German voice must still leave
            //a server that runs Italian exams, the same way a broken piper
            //leaves one that serves the page and the text of every reply
        }
    }
}

void Server::prewarm_examiner() {
    // GeminiExaminer's httplib Client is thread_local, so one prewarm() call
    // would open a connection on ONE pool thread and leave the rest cold. The
    // jobs below hold at a barrier until every one of them is running, which
    // is what guarantees they occupy distinct threads: a job that returned
    // early would free its thread to pick up the next job off the queue and
    // warm the same connection twice.
    const std::size_t count = config_.worker_threads;
    if (count == 0) {
        return;
    }

    struct Barrier {
        std::mutex m;
        std::condition_variable cv;
        std::size_t arrived = 0;
        std::size_t finished = 0;
        bool released = false;
    };
    auto barrier = std::make_shared<Barrier>();
    //shared_ptr rather than stack locals captured by reference: both waits
    //below can time out, and on that path this frame returns while jobs are
    //still running. References would dangle; a shared owner cannot

    for (std::size_t i = 0; i < count; ++i) {
        pool_.enqueue([this, barrier, count] {
            {
                std::unique_lock<std::mutex> lock(barrier->m);
                if (++barrier->arrived == count) {
                    barrier->released = true;
                    barrier->cv.notify_all();
                    //the last job to arrive frees all of them, itself included
                }
                barrier->cv.wait_for(lock,
                                     std::chrono::seconds(kPrewarmBarrierSeconds),
                                     [&] { return barrier->released; });
                //wait_for, not wait: if the pool were ever smaller than
                //worker_threads the barrier could never fill, and a startup
                //that hangs forever is far worse than a cold first turn
            }

            examiner_->prewarm();
            //outside the lock, so all the handshakes overlap instead of
            //queueing one behind another

            {
                std::lock_guard<std::mutex> lock(barrier->m);
                ++barrier->finished;
            }
            barrier->cv.notify_all();
        });
    }

    std::unique_lock<std::mutex> lock(barrier->m);
    barrier->cv.wait_for(lock, std::chrono::seconds(kPrewarmJoinSeconds),
                         [&] { return barrier->finished == count; });
    //waited on rather than abandoned so the port does not open mid-handshake,
    //and so the log below reads in order with the rest of startup
    if (barrier->finished != count) {
        std::cerr << "examiner prewarm: only " << barrier->finished << " of "
                  << count << " workers warmed before the timeout" << std::endl;
        //the stragglers pay the handshake on their first real turn, and the
        //jobs still hold the barrier alive behind us
    }
}

crow::response Server::serve_manifest(const std::string& language)
    {
    if (!is_known_language(language)) {
        return crow::response(404);
    }

    return serve_static_file("listening/" + language + "/manifest.json", "application/json");
}

crow::response Server::serve_clip(const crow::request& req,
                                  const std::string& language,
                                  const std::string& year,
                                  const std::string& file)
    {
    if (!is_known_language(language) || !is_safe_clip_name(year, file)) {
        return crow::response(404);
    }
    //rejected before the path exists, so a rejected name is never opened

    //is_safe_clip_name has already limited this to .mp3 or .png
    const bool is_image = file.compare(file.size() - 4, 4, ".png") == 0;

    return serve_range_file(req, "listening/" + language + "/clips/" + year + "/" + file,
                            is_image ? "image/png" : "audio/mpeg");
}

crow::response Server::serve_gemini_keys()
    {
    crow::json::wvalue json;
    unsigned index = 0;
    //crow wvalue::operator[] takes unsigned, same reasoning as gemini_examiner.cpp
    for (const GeminiKeyOption& option : config_.gemini_api_keys) {
        json[index] = option.name;
        //name only - the key itself never leaves the server
        ++index;
    }
    crow::response response(json.dump());
    response.set_header("Content-Type", "application/json");
    return response;
}

crow::response Server::serve_languages()
    {
    crow::json::wvalue json;
    unsigned index = 0;
    for (const LanguagePack* pack : languages_.all()) {
        json[index]["id"] = pack->id;
        json[index]["label"] = pack->display_name;
        json[index]["translate_code"] = pack->translate_code;
        //objects rather than bare strings: the picker needs the wire value, the
        //text to show and the code the translate box switches to, and deriving
        //any of the three in JavaScript would be a second table to keep in step
        ++index;
    }
    crow::response response(json.dump());
    response.set_header("Content-Type", "application/json");
    return response;
}

crow::response Server::serve_translate(const crow::request& req)
    {
    User user;
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return std::move(*refusal);
    }
    try {
        if (!allowance_for(user).paid) {
            crow::json::wvalue json;
            json["error"] =
                "Translation is part of paid access - a class licence from your "
                "school, or your own. Speaking practice and every listening paper "
                "stay free.";
            json["reason"] = "paid_only";
            return json_response(json, 403);
            //403 with a reason rather than 402 Payment Required: Crow has no
            //402 in its status table and sends an unknown code as a 500, which
            //would read as a broken server. The reason is how a page tells
            //"not included" apart from "not allowed"
        }
    } catch (const std::exception& e) {
        std::cerr << "translate: could not check access: " << e.what() << '\n';
        return json_error(500, "something went wrong, please try again");
    }
    if (auto refusal = refuse_if_rate_limited(
            "translate:" + std::to_string(user.id), 30, 60)) {
        return std::move(*refusal);
    }
    //after the paid check: a free account should hear why it cannot translate,
    //not that it is translating too fast

    if (config_.translate_api_key.empty()) {
        return json_error(503, "Translation is not configured on this server.");
        //said out loud rather than attempted with an empty key, which would come
        //back as an opaque 403 from Google and read like a broken feature
    }

    crow::json::rvalue parsed = crow::json::load(req.body);
    if (!parsed) {
        return json_error(400, "Request body was not valid JSON.");
    }

    if (!parsed.has("text") || parsed["text"].t() != crow::json::type::String ||
        !parsed.has("source") || parsed["source"].t() != crow::json::type::String ||
        !parsed.has("target") || parsed["target"].t() != crow::json::type::String) {
        return json_error(400, "Expected text, source and target strings.");
        //both the presence and the type are checked, same as protocol.cpp does:
        //rvalue::operator[] throws on a missing key and .s() on a wrong type
    }

    const std::string text(parsed["text"].s());
    const std::string source(parsed["source"].s());
    const std::string target(parsed["target"].s());

    if (text.empty()) {
        return json_error(400, "Nothing to translate.");
    }
    if (text.size() > kMaxTranslateChars) {
        return json_error(400, "That is too long - translate a sentence at a time.");
    }
    if (!is_supported_language(source) || !is_supported_language(target) ||
        source == target) {
        return json_error(400, "Unsupported language pair.");
    }

    try {
        const std::string translation =
            translate_text(config_.translate_api_key, text, source, target);

        crow::json::wvalue json;
        json["translation"] = translation;
        crow::response response(json.dump());
        response.set_header("Content-Type", "application/json");
        return response;
    } catch (const std::exception& e) {
        std::cerr << "translate route: " << e.what() << std::endl;
        return json_error(502, "The translation service could not be reached.");
        //the detail goes to the operator's log and a fixed string to the page,
        //the same split the examiner path uses for its failures
    }
}

std::optional<crow::response> Server::refuse_if_rate_limited(
    const std::string& key, int capacity, int window_seconds) {
    if (limiter_.allow(key, capacity, std::chrono::seconds(window_seconds))) {
        return std::nullopt;
    }
    crow::response response = json_error(429, "too many requests - wait a moment and try again");
    response.set_header("Retry-After", std::to_string(window_seconds));
    return response;
}

std::optional<User> Server::user_for_request(const crow::request& req) {
    auto& ctx = app_.get_context<crow::CookieParser>(req);
    const std::string token = ctx.get_cookie(kSessionCookie);
    if (token.empty()) {
        return std::nullopt;
    }
    return store_->user_for_auth_token(token);
}

crow::response Server::serve_auth_login() {
    if (config_.google_client_id.empty()) {
        return crow::response(503,
            "Sign-in is not configured on this server. "
            "Set GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET.");
        //a plain sentence rather than a stack trace: the person who sees this
        //is the one who has to go and set it
    }

    const std::string verifier = auth::random_token(32);
    const std::string state = login_states_.create(verifier);

    crow::response response(302);
    response.set_header("Location",
        auth::authorize_url(config_, state, auth::pkce_challenge(verifier)));
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response Server::serve_auth_callback(const crow::request& req) {
    const std::string error = req.url_params.get("error")
                                  ? req.url_params.get("error") : "";
    if (!error.empty()) {
        std::cerr << "oauth: Google returned error " << error << '\n';
        return auth_error_page("Google did not complete the sign-in.");
        //the commonest one is access_denied, which just means the person
        //pressed cancel - not something to show them a code for
    }

    const std::string code = req.url_params.get("code")
                                 ? req.url_params.get("code") : "";
    const std::string state = req.url_params.get("state")
                                  ? req.url_params.get("state") : "";
    if (code.empty() || state.empty()) {
        return auth_error_page("That sign-in link was incomplete.");
    }

    auth::PendingLogin pending;
    if (!login_states_.take(state, pending)) {
        return auth_error_page(
            "That sign-in link has expired or was already used. "
            "Please try signing in again.");
        //single use and time limited, so a bookmarked or replayed callback
        //lands here rather than minting a second session
    }

    const auth::SignInResult result =
        auth::exchange_code(config_, code, pending.verifier);
    if (!result.ok) {
        return auth_error_page(result.error);
    }

    try {
        const bool is_teacher = is_teacher_email(config_, result.profile.email);
        const User user = store_->upsert_google_user(result.profile, is_teacher);
        persist_quietly("invite claim", [&] {
            const int joined = store_->claim_invites(user);
            if (joined > 0) {
                std::cerr << "oauth: " << user.email << " joined " << joined
                          << " class(es) from invites\n";
            }
        });
        //every sign-in, not only the first: a teacher can add an address to a
        //class after that student already has an account and a live cookie.
        //Quietly, because a failed claim must not cost the student their login
        const std::string token = store_->create_auth_session(
            user.id, req.get_header_value("User-Agent"));

        crow::response response(302);
        auto& ctx = app_.get_context<crow::CookieParser>(req);
        ctx.set_cookie(kSessionCookie, token)
            .httponly()
            .path("/")
            .max_age(60 * 60 * 24 * 30)
            .same_site(crow::CookieParser::Cookie::SameSitePolicy::Lax);
        //Lax rather than Strict: the cookie has to survive the redirect back
        //from accounts.google.com, and Strict would drop it on exactly that hop

        if (config_.public_origin.rfind("https://", 0) == 0) {
            ctx.set_cookie(kSessionCookie, token)
                .httponly()
                .secure()
                .path("/")
                .max_age(60 * 60 * 24 * 30)
                .same_site(crow::CookieParser::Cookie::SameSitePolicy::Lax);
        }
        //Secure is decided from PUBLIC_ORIGIN rather than from a forwarded
        //header: behind a proxy the header is whatever the proxy chose to send,
        //and a Secure cookie on a plain-http dev origin never comes back at all

        response.set_header("Location", "/");
        response.set_header("Cache-Control", "no-store");
        std::cerr << "oauth: signed in " << user.email
                  << (user.is_teacher ? " (teacher)" : "") << '\n';
        return response;
    } catch (const std::exception& e) {
        std::cerr << "oauth: could not record the sign-in: " << e.what() << '\n';
        return auth_error_page("Could not complete the sign-in on this server.");
    }
}

crow::response Server::serve_auth_logout(const crow::request& req) {
    auto& ctx = app_.get_context<crow::CookieParser>(req);
    const std::string token = ctx.get_cookie(kSessionCookie);

    if (!token.empty()) {
        try {
            store_->delete_auth_session(token);
            //deleted server side, not just cleared in the browser: on a shared
            //classroom machine the cookie is the thing somebody else could reuse
        } catch (const std::exception& e) {
            std::cerr << "oauth: logout could not clear the session: "
                      << e.what() << '\n';
        }
    }

    ctx.set_cookie(kSessionCookie, "").path("/").max_age(0);

    crow::response response(302);
    response.set_header("Location", "/");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response Server::serve_me(const crow::request& req) {
    const std::optional<User> user = user_for_request(req);
    if (!user) {
        return json_error(401, "not signed in");
    }

    crow::json::wvalue json;
    json["id"] = user->id;
    json["email"] = user->email;
    json["name"] = user->display_name;
    json["is_teacher"] = user->is_teacher;
    json["year_level"] = user->year_level;
    json["subject_level"] = user->subject_level;
    json["preferred_language"] = user->preferred_language;
    json["onboarded"] = user->onboarded;

    bool has_class = false;
    try {
        has_class = store_->has_created_class(user->id);
    } catch (const std::exception& e) {
        std::cerr << "classes: could not check ownership for user " << user->id
                  << ": " << e.what() << '\n';
        has_class = true;
        //on a read failure claim they already have one: the offer is a one-off
        //nicety and showing it wrongly is worse than not showing it
    }
    json["has_created_class"] = has_class;

    try {
        const Allowance allowance = allowance_for(*user);
        json["usage"]["speaking_limit"] = allowance.limit;
        json["usage"]["speaking_used"] = allowance.used;
        json["usage"]["paid"] = allowance.paid;
        json["usage"]["paid_source"] = allowance.source;
        json["usage"]["paid_until"] = allowance.paid_until;
        json["usage"]["translation"] = allowance.paid;
        //one place a page learns what this account may do today: how much
        //speaking is left, and whether the translate box is open to it
    } catch (const std::exception& e) {
        std::cerr << "usage: could not read the allowance for user " << user->id
                  << ": " << e.what() << '\n';
        //left out rather than guessed: the page shows no count, and the
        //server still enforces the real one at Start
    }
    //the gate shows its create-a-class offer only to a teacher with no class,
    //which is what makes the offer first-time-only without a flag column

    crow::response response(json.dump());
    response.set_header("Content-Type", "application/json");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response Server::serve_set_profile(const crow::request& req) {
    User signed_in;
    if (auto refusal = refuse_unless_signed_in(req, signed_in)) {
        return std::move(*refusal);
    }
    const std::optional<User> user = std::move(signed_in);
    //through the shared check so this POST gets the same cross-origin refusal
    //as the class routes

    const crow::json::rvalue body = crow::json::load(req.body);
    if (!body) {
        return json_error(400, "expected a JSON body");
    }

    const auto field = [&body](const char* key) -> std::string {
        if (!body.has(key) || body[key].t() != crow::json::type::String) {
            return std::string();
        }
        return body[key].s();
    };
    //presence and type both, the way serve_translate reads its body: this one
    //writes to the database, so a number where a string belongs must be a 400
    //rather than something that throws out of a socket thread

    const std::string year = field("year_level");
    const std::string level = field("subject_level");
    const std::string language = field("preferred_language");

    if (!is_valid_year_level(year)) {
        return json_error(400, "pick a year level");
    }
    if (!is_valid_subject_level(level)) {
        return json_error(400, "pick a subject level");
    }
    if (languages_.find(language) == nullptr) {
        return json_error(400, "pick a language");
        //checked against the registry rather than a list written out here, so
        //adding a language cannot leave this endpoint behind
    }

    if (user->onboarded && store_->is_in_any_class(user->id)) {
        return json_error(403,
            "ask your teacher to change your year or subject level");
        //a student in a class may not move themselves between cohorts: the key
        //pool a level selects is somebody else's quota. Onboarding itself is
        //always allowed, which is why this only applies once onboarded
    }

    try {
        store_->set_profile(user->id, year, level, language);
    } catch (const std::exception& e) {
        std::cerr << "profile: could not save for user " << user->id << ": "
                  << e.what() << '\n';
        return json_error(500, "could not save that");
    }

    crow::json::wvalue json;
    json["ok"] = true;
    crow::response response(json.dump());
    response.set_header("Content-Type", "application/json");
    return response;
}

std::shared_ptr<Session> Server::find_session(crow::websocket::connection* conn) 

    {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    //lock the map of sessions_
    auto it = sessions_.find(conn);
    //.find retursn an iterator pointing at the found entry fo conn
    // or sessions_.end() if not found
    if (it == sessions_.end()) {
    //evaluate if no conn found
        return nullptr;
    //return a nullptr because the return value is still a shared_ptr object
    }
    return it->second;
    //second is the value not the key in the map
    //return the shared_ptr value of session for this conn
    }

std::shared_ptr<ConnHandle> Server::find_conn_handle(crow::websocket::connection* conn)

    {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    //a map lookup and nothing more, same as find_session
    auto it = conn_handles_.find(conn);
    if (it == conn_handles_.end()) {
        return nullptr;
        //onclose already ran for this connection, so there is nothing to send on
    }
    return it->second;
    //the caller keeps this shared_ptr alive for as long as the job lives, so the
    //handle outlives the connection even if it is destroyed mid-turn
    }

    
void Server::handle_audio(const std::shared_ptr<Session>& session,
                          const std::string& data) 
                          
    { 
    //passed in session and std::string of PCM bytes
    std::string bytes = session->take_partial_byte();
    bytes += data;
    //a websocket frame is free to split a 16-bit sample across two frames.
    //the odd byte left over last time is put back on the front of the new buffer

    const std::size_t count = bytes.size() / sizeof(std::int16_t);
    //count is the number of samples: total bytes / bytes per sample,
    //i.e. size in bytes / sizeof(std::int16_t)

    std::vector<std::int16_t> pcm(count);
    if (count > 0) {
        std::memcpy(pcm.data(), bytes.data(), count * sizeof(std::int16_t));
        //memcpy rather than reinterpret_cast: bytes.data() is a char* with
        //no 2-byte alignment guarantee, so reading it as std::int16_t* is UB
    }
    //a lone odd byte leaves count at 0, and memcpy from the possibly-null
    //data() of an empty vector is undefined even for length 0

    if (bytes.size() % sizeof(std::int16_t) != 0) {
        session->stash_partial_byte(bytes.substr(count * sizeof(std::int16_t)));
        //hold the trailing half sample back for the next frame
    }

    const bool was_full = session->audio_full();
    session->append_audio(pcm);
    //pcm now has its own heap allocated memory which is a vector of 16 bit int

    if (!was_full && session->audio_full()) {
        CROW_LOG_WARNING << "session audio buffer hit its "
                         << (Session::kMaxBufferedSamples / Session::kCaptureSampleRate)
                         << "s cap, further audio is dropped until Stop";
        //logged on the transition only, otherwise a client that keeps streaming
        //past the cap would produce a warning per frame for as long as it runs
    }
}

void Server::handle_control(crow::websocket::connection& conn,
                            const std::shared_ptr<Session>& session,
                            const std::string& data)

    {
    const crow::json::rvalue parsed = crow::json::load(data);
    //parse the text status data into a CROS::JSON rvalue

    if (!parsed) {     // ignore if malformed
        return;
    }


    const Message message = from_json(parsed); //runs function in protocol.cpp
    //convert the readable JSON into a Message object

    if (message.type == MessageType::Start) {
        std::shared_ptr<ConnHandle> handle = find_conn_handle(&conn);
        if (!handle) {
            return;
        } //the connection is already closing, so there is nowhere to send a reply

        if (session->attempt_id() != 0) {
            send_busy(handle);
            return;
            //one exam per socket. A second Start would open a second attempt
            //and ask a second opening question inside an exam already running
        }

        std::optional<User> user;
        std::optional<ClassInfo> klass;
        std::optional<ExamPlan> plan;
        std::string refusal;
        try {
            if (const auto user_id = session->user_id()) {
                user = store_->user_by_id(*user_id);
            }
            if (message.class_id > 0 && user) {
                klass = store_->class_by_id(message.class_id);
                if (klass && (klass->archived ||
                              !store_->class_role(klass->id, user->id))) {
                    klass.reset();
                }
            }
            if (klass && message.plan_id > 0) {
                plan = store_->plan_by_id(message.plan_id);
                if (!plan || plan->class_id != klass->id || plan->archived ||
                    !(plan->visible || plan->is_default)) {
                    plan.reset();
                    refusal = "that exam is not available in this class any more";
                }
                //a plan from another class, or one the teacher has put away,
                //is refused rather than quietly swapped for the default: the
                //student chose it by name
            } else if (klass) {
                for (ExamPlan& candidate : store_->class_plans(klass->id, false)) {
                    if (candidate.is_default) plan = std::move(candidate);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "start: account lookup failed: " << e.what() << '\n';
            refusal = "could not check your account, please try again";
        }

        if (refusal.empty() && config_.safety_mode != SafetyMode::Off &&
            !safety_->ready()) {
            refusal = "practice is unavailable right now, please tell your "
                      "teacher";
            std::cerr << "start refused: safety chain not ready\n";
            //checkpoint 0b. An exam that cannot be screened does not begin,
            //and it does not begin in a reduced form either: there is no
            //degraded mode here on purpose. The student-facing wording names
            //no component, because which layer is down is an operator fact
        } else if (refusal.empty() && config_.auth_required && !user) {
            refusal = "sign in to start an exam";
        } else if (refusal.empty() && user && !user->onboarded &&
                   !user->is_teacher) {
            refusal = "finish signing up before starting an exam";
            //the level chosen at sign-up is what decides which key an exam
            //spends, so an account without one must not reach the examiner.
            //Teachers are exempt: they try the exam out, they do not sit it
        } else if (refusal.empty() && message.class_id > 0 && !klass) {
            refusal = user ? "you are not in that class"
                           : "sign in to sit an exam for a class";
            //refused rather than quietly run as private practice: a student who
            //thinks their teacher will see this exam must be told otherwise
        }

        int question_limit = -1;
        if (refusal.empty() && user) {
            try {
                const Allowance allowance = allowance_for(*user);
                question_limit = allowance.limit;
                if (allowance.used >= allowance.limit) {
                    refusal = allowance.paid
                        ? "You have used all " + std::to_string(allowance.limit) +
                              " of today's speaking questions. More are available "
                              "from midnight."
                        : "You have used your " + std::to_string(allowance.limit) +
                              " free speaking questions for today. More are "
                              "available from midnight, or ask your teacher about "
                              "a class licence for unlimited practice.";
                }
            } catch (const std::exception& e) {
                std::cerr << "start: allowance lookup failed: " << e.what() << '\n';
                refusal = "could not check your account, please try again";
            }
        }
        //checked at Start so a student with nothing left is told before the
        //opening question rather than after it. The real guard is the
        //reservation in the pipeline job, which two tabs cannot both win

        if (!refusal.empty()) {
            send_error(handle, refusal);
            send_status(handle, "refused");
            return;
            //before try_begin_job, so a refused Start leaves nothing to release
        }

        if (!session->try_begin_job()) {
            send_busy(handle);
            return;
        } //a job is already in flight on this session, so refuse this message

        const LanguagePack* pack = nullptr;
        session->set_question_limit(question_limit);
        if (klass) {
            pack = languages_.find(klass->language_id);
            session->set_class_id(klass->id);
            //the class decides the language: an Italian class's exam is an
            //Italian exam whatever the picker was left on
        }
        if (pack == nullptr) {
            pack = languages_.find(message.language);
        }
        if (pack != nullptr) {
            session->set_language(pack);
        }
        if (plan) {
            session->set_plan(*plan);
            //after the language, though either order works: both rebuild the
            //prompts from the pack and the plan together
        }
        //an unknown or absent language leaves the default set on open, rather
        //than failing the Start: a stale client or a typo must still get an
        //exam. Set before the job is enqueued, because build_examiner_input()
        //reads the pack's prompts the moment the opening question is queued

        session->set_gemini_key_name(
            message.gemini_key.empty() && !config_.gemini_api_keys.empty()
                ? config_.gemini_api_keys.front().name
                : message.gemini_key);
        //an absent name is resolved to the key the examiner would fall back to
        //anyway, so the attempt and the usage counter name the key actually
        //spent rather than recording an empty string against every default turn
        //no name is set on the session, and none is read from the account
        //either: display_name used to be the fallback here, which meant a
        //signed-in student's identity reached the examiner even when they
        //never typed anything. The page does the greeting now
        //both picked once, before the first job, and reused by every later
        //turn - Stop messages carry none of these fields of their own. Set
        //before the job is enqueued, so even the opening question knows them

        persist_quietly("attempt start", [&] {
            session->set_attempt_id(store_->begin_attempt(
                session->user_id(), session->class_id(), session->language().id,
                session->gemini_key_name()));
            if (plan && session->attempt_id() != 0) {
                store_->attach_plan(session->attempt_id(), *plan,
                                    plan_to_json(*plan, true).dump());
            }
        });
        //opened here rather than on connect: the language and the class are not
        //known until Start names them, and an attempt row that cannot say which
        //exam it was is worth less than the turn it delays

        std::shared_ptr<Session> claim(session.get(), [session](Session* s) { s->end_job(); });
        //not an owner, just an RAII handle whose deleter releases the claim
        //the deleter holds session, so the Session outlives the end_job() call

        enqueue_pipeline_job(std::move(handle), session, {}, false, std::move(claim));
        return;
    }

    if (message.type == MessageType::Pause) {
        session->pause_clock(Session::Clock::now());
        return;
    }
    if (message.type == MessageType::Resume) {
        session->resume_clock(Session::Clock::now());
        return;
    }
    //neither takes the job latch, for the reason End does not: the browser
    //pauses on its own schedule and a "busy" refusal would leave the two clocks
    //disagreeing about how much of the exam is left

    if (message.type == MessageType::End) {
        if (session->attempt_id() != 0) {
            persist_quietly("attempt end (student)", [&] {
                store_->end_attempt(session->attempt_id(), "student_end");
            });
        }
        return;
        //deliberately does not take the job latch: a student may press end
        //while a turn is still in flight, and refusing that as "busy" would
        //leave the attempt to be closed as a disconnect a moment later. The
        //update is idempotent, so a turn finishing afterwards changes nothing
    }

    if (message.type != MessageType::Stop) {
        return;
    } 

    std::shared_ptr<ConnHandle> handle = find_conn_handle(&conn);
    if (!handle) {
        return;
    } //resolved before try_begin_job(), so a connection that is already closing

    if (!session->try_begin_job()) {
        send_busy(handle);
        return;
    } //refuse before take_audio(), so a rejected Stop does not discard the buffer

    std::shared_ptr<Session> claim(session.get(), [session](Session* s) { s->end_job(); });

    std::vector<std::int16_t> utterance_audio = session->take_audio();
    //take_audio() returns the completed audio buffer, clearing the session buffer
    //taking the audio on the socket thread to seperate it from any new incoming audio

    if (!session->clock_started()) {
        start_exam_clock(*session);
        //the opening question failed, so the clock never started on it. The
        //first answer starts it instead, or an exam whose first call errored
        //would never end at all
    }
    session->resume_clock(Session::Clock::now());
    //an answer is never given while paused - the page drops the mic on pause -
    //so a Stop means the exam is running again. Resuming here also stops a
    //modified page from pausing once and then answering on a clock that can
    //no longer run out
    const bool final = message.final || session->time_up(Session::Clock::now());
    //either clock running out ends the exam. The browser's is the one the
    //student watches; the server's is the one a modified page cannot stop

    enqueue_pipeline_job(std::move(handle), session, std::move(utterance_audio), true,
                         std::move(claim), final);
    //the handle rather than &conn: the job outlives handle_control, and by
    //then the raw pointer may name a destroyed connection. final turns this
    //into the last job of the session: transcribed, but never sent to the
    //examiner
}

void Server::enqueue_pipeline_job(std::shared_ptr<ConnHandle> handle,
                                  const std::shared_ptr<Session>& session,
                                  std::vector<std::int16_t> utterance_audio,
                                  bool transcribe_first,
                                  std::shared_ptr<Session> claim,
                                  bool answer_only)
                                  {
    std::vector<Turn> examiner_input = session->build_examiner_input();
    //still on Crow's socket thread, which the claim has already made exclusive
    ReplySchema reply_schema = session->reply_schema();
    //read straight after the input, which may have just ordered a set question
    //the schema has to offer as an answer

    const LanguagePack* language = &session->language();
    //snapshotted here alongside the input, for the same reason: the socket
    //thread is the one that knows the session is not mid-change. The pack
    //itself outlives every session, so the pointer stays good in the worker

    if (transcribe_first && !examiner_input.empty() &&
        examiner_input.back().role == Role::Student) {
        examiner_input.pop_back();
        //drops the previous turn's answer, this job appends a fresher one below
    }

    pool_.enqueue([this, session, transcribe_first, answer_only, language,
        handle = std::move(handle),
        job_audio = std::move(utterance_audio),
        job_input = std::move(examiner_input),
        reply_schema = std::move(reply_schema),
        claim = std::move(claim)]() mutable

    //handle is captured by value

    {
        std::string reply;
        std::vector<std::int16_t> speech;
        bool text_sent = false;
        //once the question has gone out promising audio, the client is muted
        //until a binary frame arrives, so every exit below has to send one

        using clock = std::chrono::steady_clock;
        const auto ms_since = [](clock::time_point t) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock::now() - t).count();
        };
        const auto turn_started = clock::now();
        long long stt_ms = 0;
        long long encode_ms = 0;
        long long examiner_ms = 0;
        long long tts_ms = 0;
        int student_turn = -1;
        //the index the student's answer was stored under, so the examiner's
        //reading of its tenses can be attached once the reply arrives
        //stage timings on stderr, so a slow turn names one backend. Declared
        //out here so the send below still runs after a failure

        // The examiner listens to the recording itself when it can, and this
        // job is a real answer rather than the opening question. It transcribes
        // and replies in one call, which is both one network round trip instead
        // of a local model plus a call, and a transcription made with the
        // question it just asked for context - something whisper never sees.
        //
        // One question of today's allowance is reserved before anything else,
        // because the answer decides how this job hears the student: with none
        // left the examiner is never called, so the recording goes to whisper
        // instead. A turn the examiner then fails to answer is refunded below.
        bool charged = false;
        bool quota_hit = false;
        bool examiner_answered = false;
        int questions_left = -1;
        const auto user_id = session->user_id();
        if (!answer_only && user_id && session->question_limit() >= 0) {
            try {
                const auto used = store_->reserve_usage(
                    *user_id, kSpeakingFeature, session->question_limit());
                if (used) {
                    charged = true;
                    questions_left = std::max(0, session->question_limit() - *used);
                } else {
                    quota_hit = true;
                }
            } catch (const std::exception& e) {
                std::cerr << "usage: reservation failed, turn continues "
                             "uncharged: " << e.what() << '\n';
                //open rather than closed, the same rule every other write in
                //this job follows: a database that cannot count must not cost
                //a student the turn they are in the middle of
            }
        }
        const auto refund = [&] {
            if (!charged) return;
            charged = false;
            persist_quietly("usage refund", [&] {
                store_->release_usage(*user_id, kSpeakingFeature);
            });
        };

        if (quota_hit && !transcribe_first) {
            if (session->attempt_id() != 0) {
                persist_quietly("attempt end (quota)", [&] {
                    store_->end_attempt(session->attempt_id(), "quota");
                });
            }
            send_error(handle, quota_message(session->question_limit()));
            send_status(handle, "quota");
            return;
            //the opening question itself found nothing left: Start checked, but
            //a second tab can spend the last question in between
        }

        const bool stop_after_answer = answer_only || quota_hit;
        const char* end_reason = quota_hit ? "quota" : "timer";
        const char* end_status = quota_hit ? "quota" : "ended";
        if (quota_hit) {
            send_error(handle, quota_message(session->question_limit()));
        }
        //sent ahead of the transcript, so the notice is on screen by the time
        //the page reads the status that ends the exam

        // answer_only turns are deliberately excluded: the exam clock has run
        // out and no question will be asked, so there is nothing for the
        // examiner to reply to and whisper is the cheaper way to get the words.
        // A turn with no allowance left is the same case.
        const bool examiner_listens = transcribe_first && !stop_after_answer &&
                                      examiner_->accepts_audio() &&
                                      config_.audio_input == AudioInput::Gemini &&
                                      !job_audio.empty();

        try {
            if (transcribe_first && !examiner_listens) {
                const auto stt_started = clock::now();
                std::string transcript =
                    stt_->transcribe(job_audio, language->whisper_code);
                stt_ms = ms_since(stt_started);

                if (!transcript.empty()) {
                    if (screen_student_speech(*session, handle, transcript,
                                              refund) ==
                        ScreenOutcome::TurnStopped) {
                        return;
                    }
                    //checkpoint 1, before the transcript reaches the socket,
                    //the store or the examiner's history. Nothing below this
                    //line has seen an unscreened word

                    send_transcript(handle, transcript);
                    //paint what STT heard before the reply, so a misheard
                    //answer is visible rather than only a reply that makes no sense

                    job_input.push_back(Turn{Role::Student, transcript});
                    //onto the owned snapshot, STT had not run when it was built

                    student_turn = record_student_turn(*session, transcript, stt_ms);
                    //before the move below, not after: record_answer takes the
                    //string by value and leaves the local empty

                    session->record_answer(std::move(transcript));
                    //write-through so the NEXT turn's snapshot can see this answer
                } else {
                    std::cerr << "turn skipped: empty transcript, no examiner "
                                 "request made (audio "
                              << (job_audio.size() / 16000.0) << "s, stt "
                              << stt_ms << "ms)\n";

                    refund();
                    //no examiner call is made for an empty transcript, so the
                    //question reserved for it goes back
                    send_error(handle, "Please provide a response by speaking.");
                    //distinct from the screening notice above on purpose:
                    //nothing was heard, which is a microphone or a silence
                    //problem, and telling that student their content was
                    //rejected would be both wrong and alarming
                    if (stop_after_answer) {
                        if (session->attempt_id() != 0) {
                            persist_quietly("attempt end", [&] {
                                store_->end_attempt(session->attempt_id(),
                                                    end_reason);
                            });
                        }
                        //closed here as well as on the heard path below: an
                        //unheard last answer still ended on the clock, and
                        //without this the socket closing relabels it
                        send_status(handle, end_status);
                        //nothing to re-arm for: the clock or the allowance has
                        //run out and this job was the last one, heard or not
                    } else {
                        send_examiner_text(handle, reply, false, 0);
                    }
                    return;
                    //no audio promised, so the client re-arms off the text alone
                    //an empty transcript never reaches the examiner: respond()
                }
            }

            if (stop_after_answer) {
                if (session->attempt_id() != 0) {
                    persist_quietly("attempt end", [&] {
                        store_->end_attempt(session->attempt_id(), end_reason);
                    });
                }
                send_status(handle, end_status);
                return;
                //the exam clock ran out before this answer was submitted. It
                //has been transcribed, painted and recorded above, and the job
                //stops here rather than spending a question the student has no
                //time left to hear
            }

            SpokenAnswer spoken;
            if (examiner_listens) {
                const auto encode_started = clock::now();
                const EncodedAudio encoded = encode_audio(
                    job_audio, static_cast<int>(Session::kCaptureSampleRate),
                    config_.audio_codec);
                encode_ms = ms_since(encode_started);
                spoken = SpokenAnswer{encoded.bytes, encoded.mime_type};
                //encoded here rather than on the socket thread: it is the one
                //stage that is pure CPU, and the pool is where that belongs
            }

            const auto examiner_started = clock::now();
            ExaminerReply answer;
            try {
                answer = examiner_->respond_to_audio(
                    job_input, spoken, session->gemini_key_name(), reply_schema);
                examiner_answered = true;
            } catch (const std::exception& e) {
                if (!examiner_listens) throw;
                //a text turn has no second path to fall back to

                std::cerr << "examiner failed on audio, falling back to local "
                             "transcription: " << e.what() << '\n';
                const auto stt_started = clock::now();
                answer.transcript =
                    stt_->transcribe(job_audio, language->whisper_code);
                stt_ms = ms_since(stt_started);
                //the student still sees their own words, which is most of what
                //the turn owed them. The reply is lost, and the catch below
                //sends the fixed failure string in its place
                if (!answer.transcript.empty() &&
                    screen_student_speech(*session, handle, answer.transcript,
                                          refund) ==
                        ScreenOutcome::TurnStopped) {
                    return;
                    //returns instead of rethrowing: the turn has already been
                    //closed out by the screening - refunded, the student told,
                    //and the attempt ended if it escalated - and letting the
                    //catch below overwrite that with the generic failure
                    //string would hide a disclosure behind a network error.
                    //This path is easy to miss and is exactly the one that
                    //must not be: a worrying answer does not become less
                    //worrying because the examiner call that carried it failed
                }
                if (!answer.transcript.empty()) {
                    send_transcript(handle, answer.transcript);
                    session->record_answer(answer.transcript);
                }
                throw;
            }
            examiner_ms = ms_since(examiner_started);

            if (examiner_listens) {
                if (answer.transcript.empty()) {
                    std::cerr << "examiner returned no transcript, falling back "
                                 "to local transcription\n";
                    const auto stt_started = clock::now();
                    answer.transcript =
                        stt_->transcribe(job_audio, language->whisper_code);
                    stt_ms = ms_since(stt_started);
                    //a schema hiccup costs the student their transcript but not
                    //the turn: the reply below is still good
                }

                if (!answer.transcript.empty() &&
                    screen_student_speech(*session, handle, answer.transcript,
                                          refund) ==
                        ScreenOutcome::TurnStopped) {
                    return;
                }
                //checkpoint 1 on this path is a post-hoc screen and cannot be
                //anything else: AUDIO_INPUT=gemini hands the recording to the
                //model, which transcribes AND answers in one call, so by the
                //time there is text to screen the reply already exists and was
                //generated from unscreened words. That is the mechanical
                //reason the compliant build is a cascade, and why
                //load_config() refuses this mode once AUTH_REQUIRED is on

                if (!answer.transcript.empty()) {
                    send_transcript(handle, answer.transcript);
                    //before the reply, exactly as the whisper path does: a
                    //misheard answer should be visible as itself rather than
                    //only as a reply that makes no sense

                    student_turn =
                        record_student_turn(*session, answer.transcript, stt_ms);
                    //stt_ms is 0 unless whisper was the one that produced this,
                    //which is the honest figure: the examiner's own listening
                    //is not separable from examiner_ms

                    session->record_answer(answer.transcript);
                }
            }

            const std::string& raw = answer.text;
            const std::string topic = topic_tag(raw);
            const std::string group = topic_group(topic);
            //logged only; Session maps the tag itself
            reply = clean_for_speech(raw);
            //read then stripped, so the tag never reaches the student or piper
            //cleaned here, once, ahead of all three consumers below: the model
            //slips into markdown however plainly the prompt asks for prose, and
            //the markers reached the transcript and piper's mouth alike
            //borrows the lambda's own vector, which nothing else can touch

            const SafetyVerdict outgoing = safety_->screen(
                reply, SafetyStage::ExaminerReply, language->id);
            record_safety(*session, outgoing, SafetyStage::ExaminerReply);
            if (outgoing.action != SafetyAction::Allow) {
                throw std::runtime_error("examiner reply failed screening");
                //checkpoint 3. Screened AFTER clean_for_speech, not before:
                //the student hears the cleaned text, so the cleaned text is
                //what has to be clean.
                //
                //Thrown rather than handled here so the existing catch(...)
                //backstop below sends the fixed failure string and recovers
                //the session - it already refunds the question when the
                //examiner did not answer. The asymmetry against a student
                //turn is deliberate: a generated question is regenerable and a
                //student's answer is not, so the examiner loses the turn where
                //the student would only lose the word.
                //
                //The student is never told the examiner said something
                //unusable, which is correct: that is an operator fact, not a
                //pedagogical one. One silent regeneration at the same turn
                //index is the refinement; failing the turn is the safe first
                //version
            }

            session->record_question(reply);
            session->note_question_topic(topic);
            const Session::ReplyOutcome outcome =
                session->note_examiner_reply(reply, answer);
            //the set questions and tense targets are judged on the cleaned
            //reply, the same words the student sees and hears

            int exam_seconds = 0;
            if (!session->clock_started()) {
                exam_seconds = start_exam_clock(*session);
            }
            //the opening question: the exam starts now, so the wait for the
            //first examiner call is not taken off the student's time

            send_examiner_text(handle, reply, true,
                               tts_->sample_rate(language->piper_voice_path),
                               exam_seconds, questions_left);
            text_sent = true;
            //ahead of synthesis, not after it. The question is on screen while
            //piper is still working, so the wait the student actually sees is
            //the examiner call alone rather than examiner + tts back to back

            const auto tts_started = clock::now();
            speech = tts_->synthesize(reply, language->piper_voice_path);
            tts_ms = ms_since(tts_started);

            if (session->attempt_id() != 0) {
                persist_quietly("examiner turn", [&] {
                    const std::int64_t attempt = session->attempt_id();
                    const int examiner_turn = session->next_turn_index();
                    store_->record_turn(attempt, examiner_turn,
                                        "examiner", reply, topic,
                                        0, examiner_ms, tts_ms);
                    store_->note_examiner_call(attempt, session->gemini_key_name());

                    std::vector<std::string> model_tenses;
                    for (const std::string& key : answer.question_tenses) {
                        if (is_tense_key(key)) model_tenses.push_back(key);
                    }
                    store_->record_turn_features(attempt, examiner_turn, "tense",
                                                 model_tenses, "model");
                    store_->record_turn_features(attempt, examiner_turn, "tense",
                                                 detect_tenses(language->id, reply),
                                                 "rules");

                    if (student_turn >= 0) {
                        std::vector<std::string> answer_tenses;
                        for (const std::string& key : answer.answer_tenses) {
                            if (is_tense_key(key)) answer_tenses.push_back(key);
                        }
                        store_->record_turn_features(attempt, student_turn, "tense",
                                                     answer_tenses, "model");
                        //the examiner's reading of the answer it just replied
                        //to, attached to that answer rather than to the reply
                    }

                    for (const std::int64_t id : outcome.asked) {
                        store_->mark_required_question(attempt, id, "asked",
                                                       examiner_turn);
                    }
                    for (const std::int64_t id : outcome.missed) {
                        store_->mark_required_question(attempt, id, "missed",
                                                       examiner_turn);
                    }
                    if (!outcome.evidence.empty()) {
                        std::vector<QuestionEvidence> rows;
                        rows.reserve(outcome.evidence.size());
                        for (const auto& found : outcome.evidence) {
                            rows.push_back(QuestionEvidence{
                                found.question_id, examiner_turn,
                                found.overlap, found.model_named});
                        }
                        store_->record_question_evidence(attempt, examiner_turn,
                                                         rows);
                        //written whether or not the reply closed anything: the
                        //near misses are the half of the picture the verdict
                        //cannot show
                    }
                    if (outcome.opinion_source) {
                        store_->mark_opinion_asked(attempt, examiner_turn,
                                                   *outcome.opinion_source);
                        //only on the turn that discharged it, so the row keeps
                        //the turn the teacher can go and read rather than the
                        //last turn of the exam
                    }
                });
            }
            //written here rather than beside record_question, so the row carries
            //the tts cost as well: the same three numbers the log prints below.
            //A worker is already blocked on network I/O for seconds by this
            //point, so a sub-millisecond insert to a local WAL file buys nothing
            //by being made asynchronous - and an async writer could lose turns

            std::cerr << "turn timings: audio "
                      << (job_audio.size() /
                          static_cast<double>(Session::kCaptureSampleRate))
                      << "s, stt " << stt_ms << "ms, encode " << encode_ms
                      << "ms, examiner " << examiner_ms
                      << "ms, tts " << tts_ms << "ms, total "
                      << ms_since(turn_started) << "ms, topic "
                      << (topic.empty() ? "(untagged)" : topic) << ", group "
                      << (group.empty() ? "(unrecognised)" : group) << "\n";
            //an exam full of "(untagged)" means the budget is running blind,
            //and "(unrecognised)" means a tag arrived from outside the enum
            //16000 is the capture rate the client resamples to, and the rate
            //whisper requires it
        } catch (const std::exception& e) {
            std::cerr << "turn failed, sending what we have: " << e.what() << '\n';
            if (!examiner_answered) refund();
            //the examiner never produced a question, so none was spent. A
            //failure after it did - piper, say - keeps the charge: the question
            //reached the student as text
            speech.clear();
            send_error(handle, "something went wrong on that turn");
            //a fixed student-facing string, 
        } catch (...) {
            std::cerr << "turn failed with non-std exception, sending what we have\n";
            if (!examiner_answered) refund();
            speech.clear();
            send_error(handle, "something went wrong on that turn");
            //same recovery, and reply is preserved for the same reason as above
        }

        if (text_sent) {
            send_speech(handle, speech);
            //empty if synthesis threw, which is the signal that re-arms the mic
        } else {
            send_examiner_text(handle, reply, false, 0);
            //the examiner itself failed, so reply is empty and no audio is owed
            //rate 0 because no frame follows; send_examiner_text writes the
            //field only when it is positive
        }
        //hand the result back to Crow's thread for sending. 
    }
    );
}

void Server::send_text_on_handle(const std::shared_ptr<ConnHandle>& handle,
                                 const std::string& json) {
    std::lock_guard<std::mutex> lock(handle->m);
    //same discipline as send_speech: the null check and the send are

    crow::websocket::connection* conn_ptr = handle->conn;
    if (conn_ptr == nullptr) {
        return;
    }
    conn_ptr->send_text(json);
}

void Server::send_status(const std::shared_ptr<ConnHandle>& handle,
                         const std::string& text) {
    Message message;
    message.type = MessageType::Status;
    message.payload = text;
    send_text_on_handle(handle, to_json(message).dump());
    //carries no sample_rate, so like send_error it paints nothing: the client
    //reads the payload and acts on it
}

void Server::send_busy(const std::shared_ptr<ConnHandle>& handle) {
    send_status(handle, "busy");
}

void Server::send_error(const std::shared_ptr<ConnHandle>& handle,
                        const std::string& text) {
    Message message;
    message.type = MessageType::Error;
    message.payload = text;
    send_text_on_handle(handle, to_json(message).dump());
    //carries no sample_rate, so it cannot be mistaken for a turn. It lands in
    //client.js addLog rather than addTurn, painting nothing in #transcript
}

void Server::send_transcript(const std::shared_ptr<ConnHandle>& handle,
                             const std::string& text) {
    if (text.empty()) {
        return;
        //an empty transcript is not a turn, so nothing is painted for it
    }
    Message message;
    message.type = MessageType::Transcript;
    message.payload = text;
    send_text_on_handle(handle, to_json(message).dump());
    //MessageType::Transcript was never constructed, so the client branch was
}

Server::Allowance Server::allowance_for(const User& user) {
    Allowance allowance;
    if (user.is_teacher) {
        allowance.paid = true;
        allowance.source = "teacher";
        //a teacher trying out their own plans should never meet the free
        //limit halfway through checking one
    } else {
        const PaidAccess access = store_->paid_access(user.id);
        allowance.paid = access.active;
        allowance.source = access.source;
        allowance.paid_until = access.until;
    }
    allowance.limit = allowance.paid ? config_.paid_daily_questions
                                     : config_.free_daily_questions;
    allowance.used = store_->usage_today(user.id, kSpeakingFeature);
    return allowance;
}

int Server::start_exam_clock(Session& session) {
    const int seconds =
        session.plan_duration_seconds() > 0
            ? std::clamp(session.plan_duration_seconds(), kMinExamSeconds,
                         kMaxExamSeconds)
            : config_.exam_duration_seconds;
    //clamped here as well as refused at save, because a plan stored before
    //these bounds narrowed still holds whatever length it was saved with, and
    //a plan nobody reopens would otherwise go on running past the maximum.
    //config_.exam_duration_seconds is already clamped by the config parser
    session.start_clock(Session::Clock::now(),
                        std::chrono::seconds(seconds + kClockSlackSeconds));
    return seconds;
}

// ---- safety -----------------------------------------------------------------

void Server::record_safety(Session& session, const SafetyVerdict& verdict,
                           SafetyStage stage) {
    const AdjudicationResult& adjudication = safety_->last_adjudication();
    const bool downgraded =
        adjudication.outcome == AdjudicationOutcome::Downgraded;

    if (verdict.action == SafetyAction::Allow && !downgraded) return;
    //a downgrade to Allow still gets a row. A trigger the reasoning pass
    //cleared is exactly the thing a reviewer will want to count, and it is the
    //one case where nothing else in the system would leave a trace
    if (session.attempt_id() == 0) return;
    //a practice run outside an attempt has nowhere to write the row. The
    //screening still happened and still decided the turn; only the record is
    //missing, which is the same trade every other write here makes

    persist_quietly("safety event", [&] {
        store_->record_safety_event(session.attempt_id(),
                                    session.peek_turn_index(), stage, verdict,
                                    safety_->last_adjudication());
        //peek, never take. Screening runs before the turn it screened is
        //stored, so this is that turn's own index on both checkpoints: the
        //student turn about to be written, or the examiner turn about to be
    });
}

std::string Server::safety_notice(const SafetyVerdict& verdict) {
    switch (verdict.action) {
        case SafetyAction::Mask:
            return "";
            //masking is silent - see the Mask branch of
            //screen_student_speech. Kept as a case rather than deleted so the
            //switch stays exhaustive and a future action cannot be added
            //without deciding what the student is told
        case SafetyAction::Escalate:
            //TODO(wellbeing): this wording, and the teacher workflow behind it,
            //are to be drafted with the school's wellbeing team before any
            //student other than the developer uses this. It should name the
            //school's own supports. A tool that detects a disclosure and says
            //something this generic is only barely better than one that says
            //nothing - see section 3 of docs/compliance/compliant-flow.md
            return "This practice has been stopped. If something is worrying "
                   "you, please talk to a teacher, your year adviser or the "
                   "school counsellor. Your teacher has been notified.";
        case SafetyAction::Halt:
        case SafetyAction::Allow:
            break;
    }
    return "Your answer could not be processed due to its content. Please try "
           "answering the question again.";
    //deliberately says nothing about which layer fired or what it matched: a
    //filter that explains itself is a filter that teaches you how to get past
    //it, and the category is an operator fact rather than a pedagogical one.
    //
    //It does name content as the reason, which the earlier wording did not.
    //A student whose turn stops for no stated reason reads it as the tool
    //being broken and says the same thing again; the exam continues and the
    //question has been refunded, so the retry line is the actionable half
}

Server::ScreenOutcome Server::screen_student_speech(
    Session& session, const std::shared_ptr<ConnHandle>& handle,
    std::string& transcript, const std::function<void()>& refund) {
    if (transcript.empty()) return ScreenOutcome::Continue;

    const SafetyVerdict verdict = safety_->screen(
        transcript, SafetyStage::StudentSpeech, session.language().id);
    record_safety(session, verdict, SafetyStage::StudentSpeech);

    switch (verdict.action) {
        case SafetyAction::Allow:
            transcript = verdict.text;
            return ScreenOutcome::Continue;
            //assigned rather than left alone, which matters in exactly one
            //case: a mask applied by an earlier layer, on a turn a later layer
            //stopped and the reasoning pass then cleared. The chain carries
            //the masked copy in verdict.text, and without this line the
            //ORIGINAL unmasked transcript would continue to the socket, the
            //store and the examiner. On an ordinary Allow it is a no-op

        case SafetyAction::Mask:
            transcript = verdict.text;
            return ScreenOutcome::Continue;
            //the masked copy from here on, everywhere: the socket, the store
            //and the examiner's history all see the same words. Ending a
            //language exam over a swear word punishes the disfluent, and the
            //examiner reading it back would be worse
            //
            //SILENT. The student is not told a word was hidden: the mask is
            //already visible in the transcript they can see, so a notice adds
            //nothing except a reprimand in the middle of an exam. It also
            //stops the filter advertising its own contents, which is how a
            //student learns what to type instead. The row is still written

        case SafetyAction::Halt:
            refund();
            send_error(handle, safety_notice(verdict));
            //the examiner is never called, so the reserved question goes back
            //exactly as an examiner failure already refunds it. The exam
            //continues: one stopped turn is not a stopped exam
            return ScreenOutcome::TurnStopped;

        case SafetyAction::Escalate:
            refund();
            if (session.attempt_id() != 0) {
                persist_quietly("attempt end (escalated)", [&] {
                    store_->end_attempt(session.attempt_id(), "escalated");
                });
            }
            send_error(handle, safety_notice(verdict));
            send_status(handle, "ended");
            //never silent. An escalated turn that looked to the student like a
            //network error is the failure mode Child Safe Standard 8 exists to
            //prevent, so the status goes out and the page leaves the exam
            return ScreenOutcome::TurnStopped;
    }
    return ScreenOutcome::Continue;
}

int Server::record_student_turn(Session& session, const std::string& text,
                                long long stt_ms) {
    if (session.attempt_id() == 0) {
        return -1;
    }
    int index = -1;
    persist_quietly("student turn", [&] {
        const int turn = session.next_turn_index();
        store_->record_turn(session.attempt_id(), turn, "student", text, "",
                            stt_ms, 0, 0);
        store_->record_turn_features(session.attempt_id(), turn, "tense",
                                     detect_tenses(session.language().id, text),
                                     "rules");
        index = turn;
    });
    return index;
}

void Server::send_examiner_text(const std::shared_ptr<ConnHandle>& handle,
                                const std::string& reply,
                                bool speech_follows,
                                int sample_rate,
                                int exam_seconds,
                                int questions_left) {
    Message message; //create Message Object
    message.type = MessageType::ExaminerText; //Set Message.type to Examiner Text
    message.payload = reply; //set payload to examiners reply
    message.exam_seconds = exam_seconds;
    message.questions_left = questions_left;
    if (speech_follows) {
        message.sample_rate = sample_rate;
        //tell the browser what rate the PCM frame that follows was produced at.
        //Taken from the backend rather than from the samples, because this is
        //now sent BEFORE synthesis runs - there are no samples to measure yet.
        //Its presence is also how the client knows to hold the mic muted until
        //the binary frame lands, so it must not be set on a text-only turn
        //passed in rather than read from tts_ here: the rate belongs to the
        //session's voice, and this function has no business knowing which
        //voice that is. Two languages on one server report different rates
    }
    send_text_on_handle(handle, to_json(message).dump());
}

void Server::send_speech(const std::shared_ptr<ConnHandle>& handle,
                         const std::vector<std::int16_t>& speech) {
    std::lock_guard<std::mutex> lock(handle->m);
    //the connection own mutex, not sessions_mutex_: it stops onclose letting
    //~Connection run mid-write without stalling every other connection

    crow::websocket::connection* conn_ptr = handle->conn;
    if (conn_ptr == nullptr) {
        return;
        //onclose already ran, the connection is gone and the turn is dropped.
        //Nothing to re-arm: the client that owned it is no longer listening
    }

    if (speech.empty()) {
        conn_ptr->send_binary(std::string());
        //a zero length frame, deliberately, not nothing. playAudio treats a
        //length of 0 as "no audio to wait for" and hands the turn back to the
        //student, which is the only way out once the text promised audio
        return;
    }

    const char* bytes = reinterpret_cast<const char*>(speech.data());
    //speech.data() returns a const std::int16_t* to sample 0 of the audio
    //reinterpret that pointer as a const char* so the samples are viewed as raw bytes

    const std::size_t byte_count = speech.size() * sizeof(std::int16_t);
    //byte_count is the total number of bytes in the audio
    //number of bytes = number of samples * bytes per sample

    conn_ptr->send_binary(std::string(bytes, byte_count));
    //a std::string built from the byte range as a container, not text,
    //then sent down the socket as a binary frame
}
} // namespace sim

