#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sim/tts.hpp"

namespace sim {

class PiperTTS : public InterfaceTTS {
public:
    explicit PiperTTS(std::string model_path,
                      std::vector<std::string> voice_paths = {});
    //model_path is the voice used when a call names none, which keeps a caller
    //that knows nothing about languages working. voice_paths are the rest, read
    //here only so their sample rates are cached before any turn needs one

    std::vector<std::int16_t> synthesize(const std::string& text,
                                         const std::string& voice_path) override;

    int sample_rate(const std::string& voice_path) const override;
    //declared because InterfaceTTS::sample_rate is pure virtual: without it
    //PiperTTS stays abstract and make_unique<PiperTTS> does not compile

private:
    std::string model_path_;
    std::map<std::string, int> sample_rates_;
    //voice path -> rate, each read once from <voice>.onnx.json at construction
    //because piper takes the rate from the voice's own config. Reading it per
    //turn would open a file on the path a student is waiting on
    int default_sample_rate_ = 22050;
    //model_path_'s own rate, and the answer when a voice is unknown. 22050 is
    //piper's own fallback
};

}  // namespace sim
