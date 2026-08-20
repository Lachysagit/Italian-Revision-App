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
    if (message.sample_rate > 0) {
        json["sample_rate"] = message.sample_rate;
        //the browser cannot guess the rate piper produced, so it is sent
        //alongside the text and used to build the playback buffer
    }
    return json;
}

Message from_json(const crow::json::rvalue& json) {
    //Readable CROW::JSON object passed in
    Message message; //Create a Message Object
    message.type = MessageType::Error;
    //default to Error: crow::json::load only rejects malformed syntax, so a
    //well formed message with no "type" still reaches here. operator[] throws
    //on a missing key and .s() throws on a non-string, and both would escape
    //onto the socket thread, so every field is checked before it is read

    if (json.has("type") && json["type"].t() == crow::json::type::String) {
        message.type = type_from_string(json["type"].s());
        //access type field of JSON object and use .s() to extract is as string
        //save it to message.type
    }

    if (json.has("payload") && json["payload"].t() == crow::json::type::String) {
        message.payload = json["payload"].s();
        //pull the JSON payload field out as a string an put it into Message Object
    }

    return message;
}

}  // namespace sim