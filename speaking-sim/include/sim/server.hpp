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
    crow::response serve_stylesheet();
    
    std::string load_system_prompt();
    std::string system_prompt_;

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
                              std::shared_ptr<Session> claim);
    void send_busy(const std::shared_ptr<ConnHandle>& handle);


    void send_error(const std::shared_ptr<ConnHandle>& handle,
                    const std::string& text);
 

    void send_transcript(const std::shared_ptr<ConnHandle>& handle,
                         const std::string& text);
    

    void send_examiner_result(const std::shared_ptr<ConnHandle>& handle,
                              const std::string& reply,
                              const std::vector<std::int16_t>& speech);

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
