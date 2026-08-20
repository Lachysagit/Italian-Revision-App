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

//MessageType type;
//std::string payload;

crow::json::wvalue to_json(const Message& message) {
    //message object is passed in
    crow::json::wvalue json; //create a writeable CROW::JSON object
    json["type"] = type_to_string(message.type);
    //type to string() maps the int value of the enums type value to a string
    //this value is stored on the key "type" in JSON
    json["payload"] = message.payload;
    //payload is already a string so its inputted straight into the JSON
    return json;
}

Message from_json(const crow::json::rvalue& json) {
    //Readable CROW::JSON object passed in
    Message message; //Create a Message Object
    message.type = type_from_string(json["type"].s());
    //access type field of JSON object and use .s() to extract is as string
    //save it to message.type
    message.payload = json["payload"].s();
    //pull the JSON payload field out as a string an put it into Message Object
    return message;
}

}  // namespace sim