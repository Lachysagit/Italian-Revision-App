#include "sim/examiner/gemini_examiner.hpp"

#include <iostream>
#include <utility>

namespace sim {

GeminiExaminer::GeminiExaminer(std::string api_key): api_key_(std::move(api_key)) {
}

std::string GeminiExaminer::respond(const std::vector<Turn>& history) {
    std::cerr << "GeminiExaminer::respond not implemented\n";
    return "placeholder examiner question";
}

}  // namespace sim
