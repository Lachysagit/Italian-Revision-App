#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

class InterfaceSTT {
public:

    virtual ~InterfaceSTT() = default;

    virtual std::string transcribe(const std::vector<std::int16_t>& pcm,
                                   const std::string& language_code) = 0;
    //the language is per call rather than per instance: whisper takes it as a
    //parameter and its model is multilingual, so one context serves every
    //language and two instances would only duplicate a 148MB model

};

}  // namespace sim
