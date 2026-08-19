#pragma once

#include <memory>

#include "crow.h"

#include "sim/config.hpp"
#include "sim/examiner.hpp"
#include "sim/stt.hpp"
#include "sim/tts.hpp"
#include "sim/worker.hpp"

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
    
    std::mutex sessions_mutex_;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<Session>> sessions_;

    std::shared_ptr<Session> find_session(crow::websocket::connection* conn);

    std::unique_ptr<InterfaceSTT> stt_;
    std::unique_ptr<InterfaceExaminer> examiner_;
    std::unique_ptr<InterfaceTTS> tts_;
};

}  // namespace sim