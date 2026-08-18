#include "sim/protocol.hpp"

#include <string>

namespace sim {

namespace {

std::string type_to_string(MessageType type) {
    switch (type) {
        case MessageType::Start:        return "start";
        case MessageType::Stop:         return "stop";
        case MessageType::Status:       return "status";
        case MessageType::Transcript:   return "transcript";
        case MessageType::ExaminerText: return "examiner_text";
        case MessageType::Error:        return "error";
    }
    return "error";
}

MessageType type_from_string(const std::string& text) {
    if (text == "start")         return MessageType::Start;
    if (text == "stop")          return MessageType::Stop;
    if (text == "status")        return MessageType::Status;
    if (text == "transcript")    return MessageType::Transcript;
    if (text == "examiner_text") return MessageType::ExaminerText;
    return MessageType::Error;
}

}  // namespacenamespace sim {

namespace {

std::string type_to_string(MessageType type) {
    switch (type) {
        case MessageType::Start:        return "start";
        case MessageType::Stop:         return "stop";
        case MessageType::Status:       return "status";
        case MessageType::Transcript:   return "transcript";
        case MessageType::ExaminerText: return "examiner_text";
        case MessageType::Error:        return "error";
    }
    return "error";
}

MessageType type_from_string(const std::string& text) {
    if (text == "start")         return MessageType::Start;
    if (text == "stop")          return MessageType::Stop;
    if (text == "status")        return MessageType::Status;
    if (text == "transcript")    return MessageType::Transcript;
    if (text == "examiner_text") return MessageType::ExaminerText;
    return MessageType::Error;
}

}  // namespace

crow::json::wvalue to_json(const Message& message) {
    crow::json::wvalue json;
    json["type"] = type_to_string(message.type);
    json["payload"] = message.payload;
    return json;
}

Message from_json(const crow::json::rvalue& json) {
    Message message;
    message.type = type_from_string(json["type"].s());
    message.payload = json["payload"].s();
    return message;
}

}  // namespace sim