#include "sim/examiner/hailo_examiner.hpp"

#include <iostream>
#include <utility>

namespace sim {

HailoExaminer::HailoExaminer(std::string ollama_url): ollama_url_(std::move(ollama_url)) {
} // constructor

std::string HailoExaminer::respond(const std::vector<Turn>& history,
                                    const std::string& gemini_key_name) {
    (void)history;
    (void)gemini_key_name;
    //deliberate stub: the parameters are named for the signature they use,
    //and discarded explicitly so the intent is not mistaken for an oversight
    //gemini_key_name has no meaning for this backend, which has no key concept
    std::cerr << "HailoExaminer::respond not implemented\n";
    return "placeholder examiner question";
}

}  // namespace sim
