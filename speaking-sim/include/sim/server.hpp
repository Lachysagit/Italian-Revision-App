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
#include "sim/stt.hpp"
#include "sim/tts.hpp"
#include "sim/worker.hpp"
#include "sim/session.hpp"

namespace sim {

//Liveness handle for one websocket connection, shared between Crow's socket
//thread and the worker that sends the turn back.
//Crow v1.3.3 gives handlers a crow::websocket::connection&, a pure-virtual base
//with no enable_shared_from_this and no way to reach the shared_ptr Crow owns
//internally, so a send cannot hold the connection alive by refcount. Crow's own
//anchor_ weak_ptr does not help either: it guards the work asio::post() defers,
//but send_text() has to dereference the connection to read anchor_ in the first
//place, so a dead pointer is already undefined behaviour at the call site.
//Liveness therefore has to be tracked here. m is held for the whole send; conn
//is nulled by onclose, which must take m to do it and so cannot return while a
//send is in flight. ~Connection cannot start until onclose returns, so the send
//always completes before the connection is destroyed.
//
//m also fixes frame ORDER, which is a separate problem from lifetime.
//send_text() does not write the socket on the calling thread: it builds a
//message and hands it to asio::post() on the connection's own io_context, and
//Crow binds each connection to exactly one io_context run by exactly one
//thread. So concurrent send_text() calls do not race on write_buffers_ - but
//two posts issued from different threads have no defined relative order, so a
//status frame from a socket thread could be interleaved between a worker's
//examiner text and the PCM frame that text announces. Holding m across every
//send makes each connection's frames totally ordered, because the posts
//themselves are serialised.
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
    crow::response serve_index();
    crow::response serve_client_script();
    
    std::string load_system_prompt();
    std::string system_prompt_;

    std::shared_ptr<Session> find_session(crow::websocket::connection* conn);
    std::shared_ptr<ConnHandle> find_conn_handle(crow::websocket::connection* conn);
    //resolved on the socket thread when a job is enqueued, then carried by the
    //job itself. The send path never looks a handle up, so it never needs the
    //map mutex

    void handle_audio(const std::shared_ptr<Session>& session, const std::string& data);
    void handle_control(crow::websocket::connection& conn,
                        const std::shared_ptr<Session>& session,
                        const std::string& data);
    void enqueue_pipeline_job(std::shared_ptr<ConnHandle> handle,
                              const std::shared_ptr<Session>& session,
                              std::vector<std::int16_t> utterance_audio,
                              bool transcribe_first,
                              std::shared_ptr<Session> claim);
    void send_busy(const std::shared_ptr<ConnHandle>& handle);
    //tells the client its turn was refused because a job is already in flight.
    //Takes the handle rather than the connection so it goes out under the same
    //per-connection mutex as every other send: a bare conn.send_text() posted
    //a frame that could land BETWEEN a worker's examiner text and the PCM frame
    //it describes, and the client arms its mic on a busy status

    void send_transcript(const std::shared_ptr<ConnHandle>& handle,
                         const std::string& text);
    //what STT heard, so the student can see their own answer. MessageType
    //Transcript was declared and never constructed, leaving the matching
    //client.js branch dormant and #transcript showing the examiner side only

    void send_examiner_result(const std::shared_ptr<ConnHandle>& handle,
                              const std::string& reply,
                              const std::vector<std::int16_t>& speech);

    static void send_text_on_handle(const std::shared_ptr<ConnHandle>& handle,
                                    const std::string& json);
    //the single-frame case of the locking discipline documented on ConnHandle

    //guards the two maps below and nothing else. It is a map mutex again: no
    //send is performed while it is held, so a turn on one connection no longer
    //serialises the socket threads of every other connection behind it
    std::mutex sessions_mutex_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<Session>> sessions_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<ConnHandle>> conn_handles_;
    //kept parallel to sessions_ rather than folded into Session, which owns
    //transcript and audio only and no connection state. Both maps are written
    //in onopen and onclose alone, in each case inside one lock scope, so they
    //cannot drift apart


    std::unique_ptr<InterfaceSTT> stt_;
    std::unique_ptr<InterfaceExaminer> examiner_;
    std::unique_ptr<InterfaceTTS> tts_;

    //Declared last on purpose: members are destroyed in reverse declaration
    //order, so ~WorkerPool (which drains queued jobs before joining) runs
    //first and every backend those jobs touch is still alive.
    WorkerPool pool_;
};

}  // namespace sim
