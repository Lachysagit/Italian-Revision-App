#pragma once

#include <string>
#include <vector>

#include "sim/config.hpp"
#include "sim/examiner.hpp"

namespace sim {

//Everything about the call that config.cpp can change, grouped so the
//constructor does not take four strings in an order nobody can remember.
struct GeminiSettings {
    std::string model;
    std::string thinking_level;
    std::string opening_thinking_level;
};

class GeminiExaminer : public InterfaceExaminer {
public:
    GeminiExaminer(std::vector<GeminiKeyOption> keys, GeminiSettings settings);

    std::string respond(const std::vector<Turn>& history,
                        const std::string& gemini_key_name) override;

    ExaminerReply respond_to_audio(const std::vector<Turn>& history,
                                   const SpokenAnswer& answer,
                                   const std::string& gemini_key_name) override;

    bool accepts_audio() const override { return true; }

    void prewarm() override;
    //the httplib Client is thread_local, so this has to run ON each worker
    //thread to be worth anything - see Server::prewarm_examiner()

private:
    //the one place a request is built and sent. Both entry points above land
    //here, so the two cannot drift apart in what they ask for
    ExaminerReply call(const std::vector<Turn>& history,
                       const SpokenAnswer& answer,
                       const std::string& gemini_key_name);

    const std::string& key_for(const std::string& gemini_key_name) const;

    std::vector<GeminiKeyOption> keys_;
    GeminiSettings settings_;
};

}  // namespace sim
