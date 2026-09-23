#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

class InterfaceTTS {
public:
    virtual ~InterfaceTTS() = default;

    virtual std::vector<std::int16_t> synthesize(const std::string& text,
                                                 const std::string& voice_path) = 0;
    //the voice is per call rather than per instance: piper spawns a process per
    //turn with the model as an argument, so switching voices between sessions
    //costs nothing and needs no second backend

    virtual int sample_rate(const std::string& voice_path) const = 0;
    //the rate synthesize() produces for that voice. The browser has no way to
    //infer it, and playing it back at the AudioContext rate shifts the pitch.
    //Per voice because each carries its own rate in its .onnx.json
};

}  // namespace sim
