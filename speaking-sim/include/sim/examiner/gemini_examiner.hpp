#pragma once

#include <string>
#include <vector>

#include "sim/config.hpp"
#include "sim/examiner.hpp"

namespace sim {

class GeminiExaminer : public InterfaceExaminer {
public:
    explicit GeminiExaminer(std::vector<GeminiKeyOption> keys);

    std::string respond(const std::vector<Turn>& history,
                        const std::string& gemini_key_name) override;

    void prewarm() override;
    //the httplib Client is thread_local, so this has to run ON each worker
    //thread to be worth anything - see Server::prewarm_examiner()

private:
    const std::string& key_for(const std::string& gemini_key_name) const;

    std::vector<GeminiKeyOption> keys_;
};

}  // namespace sim
