#include "sim/examiner/hailo_examiner.hpp"

#include <iostream>
#include <utility>

namespace sim {

HailoExaminer::HailoExaminer(std::string ollama_url): ollama_url_(std::move(ollama_url)) {
}

std::string HailoExaminer::respond(const std::vector<Turn>& history) {
    (void)history;
    //deliberate stub: the parameter is named for the signature it will use,
    //and discarded explicitly so the intent is not mistaken for an oversight
    std::cerr << "HailoExaminer::respond not implemented\n";
    return "placeholder examiner question";
}

}  // namespace sim
