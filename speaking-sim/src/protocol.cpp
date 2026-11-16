#include "sim/protocol.hpp"

#include <string>

#include "sim/http_util.hpp"

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

crow::json::wvalue to_json(const Message& message) {
    crow::json::wvalue json;
    json["type"] = type_to_string(message.type);
    json["payload"] = message.payload;

    // Every field below is optional and is left out, rather than sent as a
    // zero or an empty string, when it has nothing to say.
    if (message.sample_rate > 0) {
        json["sample_rate"] = message.sample_rate;
        //the browser cannot guess the rate piper produced, so it is sent
        //alongside the text and used to build the playback buffer
    }
    if (!message.language.empty()) json["language"] = message.language;
    if (!message.gemini_key.empty()) json["gemini_key"] = message.gemini_key;
    if (message.exam_seconds > 0) json["exam_seconds"] = message.exam_seconds;
    if (message.questions_left >= 0) json["questions_left"] = message.questions_left;
    if (message.plan_id > 0) json["plan_id"] = message.plan_id;
    if (message.class_id > 0) json["class_id"] = message.class_id;
    if (message.final) json["final"] = true;
    return json;
}

Message from_json(const crow::json::rvalue& json) {
    Message message;
    message.type = MessageType::Error;
    //default to Error: crow::json::load only rejects malformed syntax, so a
    //well formed message with no "type" reaches here

    if (!json || json.t() != crow::json::type::Object) {
        return message;
        //a bare number, string or list is valid JSON and not a message.
        //rvalue::has throws on anything that is not a container, and that
        //exception used to escape onto Crow's socket thread and be logged as a
        //"Worker Crash" for a frame any client can send
    }

    // Presence AND type on every field: rvalue::operator[] throws on a missing
    // key and .s()/.i() on a wrong type. A field of the wrong shape is left at
    // its default, so a client that sends one loses that field, not the exam.
    // Strings go through string_field; integers have their own reader here
    // because a message id must also refuse a fraction.
    const auto integer = [&json](const char* key) -> std::int64_t {
        if (!json.has(key) || json[key].t() != crow::json::type::Number ||
            json[key].nt() == crow::json::num_type::Floating_point) {
            return 0;
            //a fraction is refused rather than handed to .i(), which would
            //quietly truncate 12.9 to class 12. Zero is "no class", "no plan"
        }
        return json[key].i();
    };

    const std::string type = string_field(json, "type");
    if (!type.empty()) message.type = type_from_string(type);
    message.payload = string_field(json, "payload");
    message.language = string_field(json, "language");
    message.gemini_key = string_field(json, "gemini_key");
    message.class_id = integer("class_id");
    message.plan_id = integer("plan_id");
    message.final = json.has("final") && json["final"].t() == crow::json::type::True;
    //crow gives true and false separate types, so testing for True is the
    //whole check

    //"student_name" is ignored if a stale client still sends it: there is
    //nothing to decode it into, so the key falls on the floor here rather
    //than reaching Session or a prompt

    return message;
}

}  // namespace sim
