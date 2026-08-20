#pragma once

#include <string>
#include <vector>

#include "sim/examiner.hpp"

namespace sim {

class HailoExaminer : public InterfaceExaminer { //inheritance
public:
    explicit HailoExaminer(std::string ollama_url);

    std::string respond(const std::vector<Turn>& history) override;
    //override pure virtual respond method of base class

private:
    std::string ollama_url_;
};

}  // namespace sim
