#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

#include "crow.h"
#include "crow/middlewares/cookie_parser.h"

#include "sim/config.hpp"
#include "sim/examiner.hpp"
#include "sim/language.hpp"
#include "sim/rate_limit.hpp"
#include "sim/safety/safety_chain.hpp"
#include "sim/stt.hpp"
#include "sim/tts.hpp"
#include "sim/worker.hpp"
#include "sim/session.hpp"
#include "sim/store.hpp"
#include "sim/auth/google_oauth.hpp"

namespace sim {


struct ConnHandle {
    std::mutex m;
    crow::websocket::connection* conn = nullptr;
};

class Server {
public:
    Server(Config config,
           std::unique_ptr<InterfaceSTT> stt,
           std::unique_ptr<InterfaceExaminer> examiner,
           std::unique_ptr<InterfaceTTS> tts,
           std::unique_ptr<Store> store,
           std::unique_ptr<SafetyChain> safety
        );

    void run();
    ~Server();
    //declared because of retention_thread_ below: the thread has to be told to
    //stop and joined before store_ goes, and the implicit destructor would do
    //neither

private:
    void retention_loop();
    //one pass now, then one a day until shutdown. Lives on its own thread
    //rather than in the pool: a purge must not be able to take a worker away
    //from a student mid-turn, and it is a few bounded DELETEs once a day
    Config config_;
    using App = crow::App<crow::CookieParser>;
    App app_;
    //CookieParser parses the session cookie and sets it on the way out; it
    //gates nothing. Which routes need a user is decided per route by
    //require_user, so the route table stays the place that says so rather than
    //a prefix test buried in a middleware
    crow::response serve_manifest(const std::string& language);

    crow::response serve_clip(const crow::request& req,
                              const std::string& language,
                              const std::string& year,
                              const std::string& file);
    //language, year and file come straight off the URL, so they are validated
    //against an allowlist before the path is built

    crow::response serve_gemini_keys();
    //names only, never the keys themselves - this is a plain unauthenticated
    //GET a browser tab can fire, so it must be safe to expose to anyone who
    //can already reach the server

    crow::response serve_languages();
    //what the speaking page's picker is built from: id, label and translate
    //code per language. Objects rather than bare strings because the picker
    //needs the wire value, the display name and the translate code at once

    crow::response serve_translate(const crow::request& req);
    //the translate box. Deliberately plain HTTP rather than a /ws message: the
    //socket only exists while a session is running, and the session's one-job
    //latch would refuse a lookup made mid-turn as "busy"

    LanguageRegistry languages_;
    //every language's prompts, question bank, voice and per-language strings,
    //loaded once at startup and const from there on. Sessions hold bare
    //pointers into it, so it must outlive them - which a member of Server does.
    //The prompts, the bank and load_prompt all used to live here as separate
    //members; they moved into the pack when a second language needed its own

    crow::response serve_auth_login();
    crow::response serve_auth_callback(const crow::request& req);
    crow::response serve_auth_logout(const crow::request& req);
    crow::response serve_me(const crow::request& req);
    crow::response serve_set_profile(const crow::request& req);
    //sign-in. The token exchange in the callback is an outbound HTTPS call on a
    //crow socket thread, like /api/translate: once per sign-in, with explicit
    //timeouts, and never taking a worker from a student mid-turn

    std::optional<User> user_for_request(const crow::request& req);
    //the cookie -> user lookup every protected route starts with. nullopt means
    //not signed in, which each caller turns into its own 401 or redirect

    // ---- classes: src/class_api.cpp ---------------------------------------
    //kept in their own file so the route table here stays readable, and so the
    //whole group can move into a separate API service without untangling it

    void register_class_routes();

    std::optional<crow::response> refuse_unless_signed_in(
        const crow::request& req, User& user);
    //nullopt means go ahead, with user filled in; a response means send that
    //instead. Also refuses a state-changing request from another origin, so a
    //route cannot forget the check by forgetting to call a second helper
    std::optional<crow::response> refuse_unless_teaches(
        const crow::request& req, std::int64_t class_id,
        User& user, ClassInfo& klass);
    //the same, plus: the class exists and the caller is one of its teachers.
    //A class that is not theirs is a 404 rather than a 403, so class ids
    //cannot be probed for existence

    crow::response serve_classes(const crow::request& req);
    crow::response serve_class(const crow::request& req, std::int64_t class_id);
    crow::response serve_join_code(const crow::request& req, std::int64_t class_id);
    crow::response serve_invites(const crow::request& req, std::int64_t class_id);
    crow::response serve_revoke_invite(const crow::request& req,
                                       std::int64_t class_id,
                                       std::int64_t invite_id);
    crow::response serve_remove_member(const crow::request& req,
                                       std::int64_t class_id,
                                       std::int64_t user_id);
    crow::response serve_remove_member_fragment(const crow::request& req,
                                                std::int64_t class_id,
                                                std::int64_t user_id);
    std::optional<crow::response> refuse_unless_removable(std::int64_t class_id,
                                                          std::int64_t user_id);
    //the rule about who may be removed, held in one place so the JSON route and
    //the fragment route cannot drift apart on it
    crow::response serve_archive(const crow::request& req, std::int64_t class_id);
    crow::response serve_class_attempts(const crow::request& req,
                                        std::int64_t class_id);
    crow::response serve_class_attempts_fragment(const crow::request& req,
                                                 std::int64_t class_id);
    crow::response serve_class_members(const crow::request& req,
                                       std::int64_t class_id);
    crow::response members_fragment(std::int64_t class_id,
                                    const ClassInfo& klass);
    //shared by the GET and by a successful DELETE, so a removal answers with
    //the table redrawn by the same code rather than a second copy of it
    crow::response serve_my_attempts(const crow::request& req);
    crow::response serve_attempt(const crow::request& req, std::int64_t attempt_id);
    crow::response serve_join(const crow::request& req);

    // ---- exam plans: src/plan_api.cpp ------------------------------------

    void register_plan_routes();
    crow::response serve_class_plans(const crow::request& req, std::int64_t class_id);
    crow::response serve_plan(const crow::request& req, std::int64_t plan_id);
    crow::response serve_archive_plan(const crow::request& req, std::int64_t plan_id);
    crow::response serve_default_plan(const crow::request& req, std::int64_t class_id);
    crow::response serve_exam_options(const crow::request& req);
    crow::response serve_coverage(const crow::request& req, std::int64_t class_id);
    crow::response serve_coverage_fragment(const crow::request& req,
                                           std::int64_t class_id);

    RateLimiter limiter_;
    std::optional<crow::response> refuse_if_rate_limited(const std::string& key,
                                                         int capacity,
                                                         int window_seconds);
    //a 429 once key has made capacity requests inside the window. Keys carry
    //the route and the caller - "join:12", "login:203.0.113.4" - so one busy
    //student cannot starve another

    auth::LoginStates login_states_;
    //the PKCE verifier and state for sign-ins in flight, in memory: they live
    //for one redirect round trip

    void prewarm_tts();
    //one throwaway synthesis per distinct voice at startup, so the first real
    //turn in either language does not pay piper's cold cost. Called from run()
    //before the port is bound

    void prewarm_examiner();
    //the same idea for the examiner's HTTPS connection, but it has to run once
    //on EVERY pool thread, because the client behind it is thread_local

    std::shared_ptr<Session> find_session(crow::websocket::connection* conn);
    std::shared_ptr<ConnHandle> find_conn_handle(crow::websocket::connection* conn);
    //resolved on the socket thread when a job is enqueued, then carried by the
    //job itself. The send path never looks one up, so it needs no map mutex

    void handle_audio(const std::shared_ptr<Session>& session, const std::string& data);
    void handle_control(crow::websocket::connection& conn,
                        const std::shared_ptr<Session>& session,
                        const std::string& data);
    void enqueue_pipeline_job(std::shared_ptr<ConnHandle> handle,
                              const std::shared_ptr<Session>& session,
                              std::vector<std::int16_t> utterance_audio,
                              bool transcribe_first,
                              std::shared_ptr<Session> claim,
                              bool answer_only = false);
    //answer_only stops the job after STT: the transcript is sent and recorded,
    //and the examiner is never called. It is how the last answer of an expired
    //exam is still heard without buying the student another question
    void send_status(const std::shared_ptr<ConnHandle>& handle,
                     const std::string& text);
    void send_busy(const std::shared_ptr<ConnHandle>& handle);

    void send_error(const std::shared_ptr<ConnHandle>& handle,
                    const std::string& text);
 

    void send_transcript(const std::shared_ptr<ConnHandle>& handle,
                         const std::string& text);
    

    void send_examiner_text(const std::shared_ptr<ConnHandle>& handle,
                            const std::string& reply,
                            bool speech_follows,
                            int sample_rate,
                            int exam_seconds = 0,
                            int questions_left = -1);
    //exam_seconds rides on the opening question only, the moment the server's
    //clock starts, so the browser's countdown starts from the same instant

    struct Allowance {
        bool paid = false;
        std::string source;
        //"user" or "class" for a licence, "teacher" for a teacher account
        std::int64_t paid_until = 0;
        int limit = 0;
        int used = 0;
    };
    Allowance allowance_for(const User& user);
    //today's speaking allowance for this account and what it has spent. Also
    //what decides translation: that is part of paid access, not metered

    int start_exam_clock(Session& session);
    //the deadline is the plan's length, or the configured one, plus a few
    //seconds of slack for the question reaching the browser, whose countdown
    //starts on arrival. Returns the length in seconds, which the browser is told

    int record_student_turn(Session& session, const std::string& text,
                            long long stt_ms);
    //writes the student's answer and the rule-based tenses in it, and returns
    //the turn's index so the examiner's own labels can be added once its reply
    //arrives. -1 when the attempt is not being recorded
    //sample_rate is the session's own voice rate, passed in because two
    //languages on one server produce different ones. Ignored when
    //speech_follows is false, since no frame is coming to describe
    //split off from the old send_examiner_result so the question can be painted
    //the moment the examiner returns it, rather than behind the TTS it describes

    void send_speech(const std::shared_ptr<ConnHandle>& handle,
                     const std::vector<std::int16_t>& speech);
    //always sent once send_examiner_text said audio was coming, empty included:
    //the client re-arms the mic off this frame and would otherwise wait forever

    static void send_text_on_handle(const std::shared_ptr<ConnHandle>& handle,
                                    const std::string& json);

    // ---- safety ----------------------------------------------------------

    enum class ScreenOutcome {
        Continue,
        //Allow or Mask. transcript now holds what everything downstream sees
        TurnStopped,
        //Halt or Escalate. The student has been told, the question refunded and
        //the attempt closed where the action called for it. The caller returns
    };

    ScreenOutcome screen_student_speech(Session& session,
                                        const std::shared_ptr<ConnHandle>& handle,
                                        std::string& transcript,
                                        const std::function<void()>& refund);
    //checkpoint 1, called before the transcript reaches the socket, the store
    //or the examiner - on all three paths that produce one. A helper rather
    //than three pasted blocks because the third path is easy to miss: the
    //whisper fallback inside the examiner's own catch also produces a
    //transcript, and an unscreened disclosure does not become less urgent
    //because the turn it arrived in was already failing

    void record_safety(Session& session, const SafetyVerdict& verdict,
                       SafetyStage stage);
    //one persist_quietly write. A database that cannot record the event must
    //not also cost the student the turn, which is the rule every other write
    //in this file follows

    static std::string safety_notice(const SafetyVerdict& verdict);
    //what the student is told. Fixed strings, chosen by action and never
    //carrying the category, the matched term or anything the layer saw


    std::mutex sessions_mutex_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<Session>> sessions_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<ConnHandle>> conn_handles_;



    std::unique_ptr<InterfaceSTT> stt_;
    std::unique_ptr<InterfaceExaminer> examiner_;
    std::unique_ptr<InterfaceTTS> tts_;

    std::unique_ptr<SafetyChain> safety_;
    //screens the student's transcript before the examiner is called and the
    //examiner's reply before anything is spoken or stored. Never null: main()
    //builds one in every mode, and the off mode is an empty chain whose
    //ready() is false, which is what stops an exam starting unscreened
    //
    //MUST stay declared AFTER examiner_. When the semantic reasoning pass is
    //configured it holds a borrowed InterfaceExaminer*, and members are
    //destroyed in reverse declaration order, so this ordering is what makes
    //that pointer valid for the chain's whole life. Moving this line above
    //examiner_ would turn the last screening call of a shutdown into a use
    //after free

    std::unique_ptr<Store> store_;
    //accounts, classes and exam history. A member of Server like languages_ so
    //it outlives every session - but unlike that registry it is mutable, which
    //is why Session must not hold a pointer to it. Session stays a pure state
    //object and Server does the writing

    WorkerPool pool_;

    std::thread retention_thread_;
    std::mutex retention_m_;
    std::condition_variable retention_wake_;
    bool retention_stopping_ = false;
    //a condition_variable rather than a sleep: a server shut down four hours
    //into the day's wait should exit then, not when the wait was due to end
};

}  // namespace sim
