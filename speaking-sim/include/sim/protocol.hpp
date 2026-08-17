#pragma once

#include <string>

#include "crow/json.h"

namespace sim {

enum class MessageType {
    Start, //from browser
    Stop, //from browser
    Status, //from server
    Transcript, //from server
    ExaminerText,
    Error,
};
    
struct Message {
    MessageType type;
    std::string payload;
};

crow::json::wvalue to_json(const Message& message);

Message from_json(const crow::json::rvalue& json);

}  // namespace sim