#pragma once

#include <string>
#include <vector>

namespace sim {

enum class Role {
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

    virtual std::string respond(const std::vector<Turn>& history) = 0;
};

}  // namespace sim