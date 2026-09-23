#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "crow.h"

#include "sim/config.hpp"
#include "sim/examiner.hpp"
#include "sim/language.hpp"
#include "sim/stt.hpp"
#include "sim/tts.hpp"
#include "sim/worker.hpp"
#include "sim/session.hpp"

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
           std::unique_ptr<InterfaceTTS> tts
        );

    void run();

private:
    Config config_;
    crow::SimpleApp app_;
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
                            int sample_rate);
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


    std::mutex sessions_mutex_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<Session>> sessions_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<ConnHandle>> conn_handles_;



    std::unique_ptr<InterfaceSTT> stt_;
    std::unique_ptr<InterfaceExaminer> examiner_;
    std::unique_ptr<InterfaceTTS> tts_;

    WorkerPool pool_;
};

}  // namespace sim
