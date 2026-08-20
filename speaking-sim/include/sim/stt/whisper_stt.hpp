#pragma once

#include <string>
#include <vector>

#include "sim/stt.hpp"

namespace sim {

class WhisperSTT : public InterfaceSTT { //inheritance
public:
    explicit WhisperSTT(std::string model_path);

    std::string transcribe(const std::vector<std::int16_t>& pcm) override;
    //override pure virtual transcribe method of base class

private:
    std::string model_path_;
};

}  // namespace sim