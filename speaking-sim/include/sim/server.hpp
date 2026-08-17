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
    WorkerPool pool_;

    std::unique_ptr<InterfaceSTT> stt_;
    std::unique_ptr<InterfaceExaminer> examiner_;
    std::unique_ptr<InterfaceTTS> tts_;
};

}  // namespace sim