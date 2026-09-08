#pragma once

#include <string>
#include <vector>

namespace sim {

enum class Role {
    System,
    Examiner,
    Student,
};

struct Turn {
    Role role;
    std::string text;
};

class InterfaceExaminer {
public:
    virtual ~InterfaceExaminer() = default;

    //gemini_key_name selects among Config::gemini_api_keys; empty means "use
    //the default". Backends without a key concept (Hailo) ignore it
    virtual std::string respond(const std::vector<Turn>& history,
                                 const std::string& gemini_key_name = "") = 0;

    //Opens whatever connection the backend needs, so the first real turn does
    //not pay for it. Called once per worker thread at startup, off the critical
    //path. Not pure: a backend with nothing to open inherits the empty default
    //rather than being forced to write one
    virtual void prewarm() {}
};

}  // namespace sim
