#pragma once

#include <cstdint>
#include <string>

#include "crow/json.h"

namespace sim {

enum class MessageType {
    Start, //from browser
    Stop, //from browser
    End, //from browser: the student pressed end rather than running out of time
    Status, //from server
    Transcript, //from server
    ExaminerText, //from server
    Error, //from server
};
    
struct Message {
    MessageType type;
    std::string payload;
    int sample_rate = 0;
    //sample rate of the binary audio frame that follows this message.
    //only written to the JSON when non-zero, so control messages are unchanged
    std::string gemini_key;
    //name of the Gemini key the settings picker chose, sent with Start only.
    //only written to the JSON when non-empty, same discipline as sample_rate
    std::string language;
    //which exam the browser's picker chose, sent with Start only. Same
    //discipline as the two below: absent rather than empty when nothing was
    //picked, which is how the server tells "run the default" from a client
    //that names a language it does not have
    std::string student_name;
    //what the examiner calls the student, sent with Start only. Same discipline
    //again: absent rather than empty when the settings field was left blank,
    //which is how the session tells "no name given" from "named nothing"
    int exam_seconds = 0;
    //from the server, on the opening question only: how long the exam runs.
    //The server's clock is the one that counts; the browser's countdown is a
    //display of it, so a page left on an old default still shows the truth
    std::int64_t class_id = 0;
    //sent with Start only: the class this exam is being sat for, or absent for
    //private practice. A claim the server checks against class_members before
    //it believes it, since it decides which teacher can read the transcript
    bool final = false;
    //sent with Stop only, and only once the exam clock has run out: transcribe
    //this answer and record it, but make no examiner request from it. The
    //browser is the only thing that knows the time, so it is the browser that
    //says so - the server keeps no clock of its own
};

crow::json::wvalue to_json(const Message& message);

Message from_json(const crow::json::rvalue& json);

}  // namespace sim
