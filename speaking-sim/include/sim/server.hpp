#pragma once

#include <memory>

#include "crow.h"

#include "sim/config.hpp"
#include "sim/examiner.hpp"
#include "sim/stt.hpp"
#include "sim/tts.hpp"
#include "sim/worker.hpp"
#include "sim/session.hpp"

namespace sim {

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
    
    std::string load_system_prompt();
    std::string system_prompt_;

    WorkerPool pool_;
    
    std::shared_ptr<Session> find_session(crow::websocket::connection* conn);

    void handle_audio(const std::shared_ptr<Session>& session, const std::string& data);
    void handle_control(crow::websocket::connection& conn,
                        const std::shared_ptr<Session>& session,
                        const std::string& data);

    std::mutex sessions_mutex_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<Session>> sessions_;


    std::unique_ptr<InterfaceSTT> stt_;
    std::unique_ptr<InterfaceExaminer> examiner_;
    std::unique_ptr<InterfaceTTS> tts_;
};

}  // namespace sim