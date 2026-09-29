#include "sim/protocol.hpp"

#include <string>

namespace sim {

namespace {

std::string type_to_string(MessageType type) {
    switch (type) {
        case MessageType::Start:        return "start";
        case MessageType::Stop:         return "stop";
        case MessageType::End:          return "end";
        case MessageType::Pause:        return "pause";
        case MessageType::Resume:       return "resume";
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
    if (text == "end")           return MessageType::End;
    if (text == "pause")         return MessageType::Pause;
    if (text == "resume")        return MessageType::Resume;
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
    if (!message.language.empty()) {
        json["language"] = message.language;
    }
    if (!message.gemini_key.empty()) {
        json["gemini_key"] = message.gemini_key;
    }
    if (!message.student_name.empty()) {
        json["student_name"] = message.student_name;
    }
    if (message.exam_seconds > 0) {
        json["exam_seconds"] = message.exam_seconds;
    }
    if (message.questions_left >= 0) {
        json["questions_left"] = message.questions_left;
    }
    if (message.plan_id > 0) {
        json["plan_id"] = message.plan_id;
    }
    if (message.class_id > 0) {
        json["class_id"] = message.class_id;
    }
    if (message.final) {
        json["final"] = true;
        //absent rather than false on an ordinary turn, the same discipline the
        //optional fields above keep
    }
    return json;
}

Message from_json(const crow::json::rvalue& json) {
    //Readable CROW::JSON object passed in
    Message message; //Create a Message Object
    message.type = MessageType::Error;
    //default to Error: crow::json::load only rejects malformed syntax, so a
    //well formed message with no "type" reaches here. Every field is checked

    if (json.has("type") && json["type"].t() == crow::json::type::String) {
        message.type = type_from_string(json["type"].s());
        //access type field of JSON object and use .s() to extract is as string
        //save it to message.type
    }

    if (json.has("payload") && json["payload"].t() == crow::json::type::String) {
        message.payload = json["payload"].s();
        //pull the JSON payload field out as a string an put it into Message Object
    }

    if (json.has("language") && json["language"].t() == crow::json::type::String) {
        message.language = json["language"].s();
        //presence AND type, like every other field: a client that sends a
        //number here leaves the language empty and gets the default, rather
        //than throwing on .s()
    }

    if (json.has("gemini_key") && json["gemini_key"].t() == crow::json::type::String) {
        message.gemini_key = json["gemini_key"].s();
    }

    if (json.has("student_name") && json["student_name"].t() == crow::json::type::String) {
        message.student_name = json["student_name"].s();
        //checked for presence AND type like every other field: a client that
        //sends a number here must leave the name empty, not throw on .s()
    }

    if (json.has("class_id") && json["class_id"].t() == crow::json::type::Number) {
        message.class_id = json["class_id"].i();
        //a string or a fraction leaves 0, which is private practice: a client
        //that sends the wrong shape loses the class, not the exam
    }

    if (json.has("plan_id") && json["plan_id"].t() == crow::json::type::Number) {
        message.plan_id = json["plan_id"].i();
    }

    if (json.has("final") && json["final"].t() == crow::json::type::True) {
        message.final = true;
        //crow gives true and false separate types, so testing for True is the
        //whole check: anything else leaves the default false standing
    }

    return message;
}

}  // namespace sim
