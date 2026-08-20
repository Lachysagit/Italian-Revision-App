#include "sim/examiner/hailo_examiner.hpp"

#include <iostream>
#include <utility>

namespace sim {

HailoExaminer::HailoExaminer(std::string ollama_url): ollama_url_(std::move(ollama_url)) {
}

std::string HailoExaminer::respond(const std::vector<Turn>& history) {
    std::cerr << "HailoExaminer::respond not implemented\n";
    return "placeholder examiner question";
}

}  // namespace sim
