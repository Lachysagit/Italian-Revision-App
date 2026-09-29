#pragma once

#include <atomic>
#include <string>

#include "sim/safety.hpp"

namespace sim {

// The remote layer: Azure AI Content Safety in Australia East, reached over a
// private endpoint from inside the VNet. Two of its operations, which map onto
// two of the department's own filters:
//
//   text:analyze      four categories, severity 0-6  -> the semantic filter
//   text:shieldPrompt jailbreak and injection         -> jailbreak prevention
//
// Only text:analyze runs on an examiner reply. Shield Prompt is about a user
// trying to subvert a system prompt, which the examiner's own output cannot
// be doing, and a second call per turn costs latency a student is sitting
// through.
//
// The resource must be created in Australia East. A Content Safety resource in
// another region is the same compliance failure as the examiner being in
// another region, and is easier to create by accident.
class AzureSafety : public InterfaceSafety {
public:
    struct Options {
        std::string endpoint;
        //https://<resource>.cognitiveservices.azure.com, the private DNS name
        std::string api_key;
        //from Key Vault at startup. Left empty when the relay authenticates
        //with its managed identity, which is the configuration to prefer:
        //a key that does not exist cannot be leaked in a log line
        std::string bearer_token;
        int halt_severity = 2;
        //0, 2, 4, 6 on the four-level scale. 2 stops anything the service is
        //willing to call low severity, which is the right setting for minors
        //and is deliberately stricter than the service default
        bool shield_prompts = true;
    };

    explicit AzureSafety(Options options);

    SafetyVerdict screen(const std::string& text,
                         SafetyStage stage,
                         const std::string& language_id) override;
    //throws on a transport or HTTP failure. The chain decides what a failure
    //means, because the answer differs by stage and this class does not know
    //the policy

    bool available() const override;
    void prewarm() override;

private:
    SafetyVerdict analyse(const std::string& text);
    SafetyVerdict shield(const std::string& text);

    Options options_;
    std::atomic<bool> healthy_{false};
};

}  // namespace sim
