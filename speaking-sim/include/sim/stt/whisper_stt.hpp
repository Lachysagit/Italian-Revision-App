#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sim/stt.hpp"

struct whisper_context;
//forward declared rather than including whisper.h: whisper_context is opaque,
//so a pointer needs no definition and includers stay free of whisper headers.

namespace sim {

class WhisperSTT : public InterfaceSTT { //inheritance
public:
    explicit WhisperSTT(std::string model_path);
    ~WhisperSTT() override;
    //the model is now owned for the lifetime of the object, so it needs a
    //destructor to release it

    WhisperSTT(const WhisperSTT&) = delete;
    WhisperSTT& operator=(const WhisperSTT&) = delete;
    WhisperSTT(WhisperSTT&&) = delete;
    WhisperSTT& operator=(WhisperSTT&&) = delete;
    //ctx_ is a raw owning pointer and mutex_ cannot be moved, so all four are
    //deleted explicitly to fail at the call site with a clear message

    std::string transcribe(const std::vector<std::int16_t>& pcm) override;
    //override pure virtual transcribe method of base class

private:
    std::string model_path_;

    whisper_context* ctx_ = nullptr;
    //loaded once in the constructor, freed once in the destructor. Null when
    //no model path was configured, which disables transcription

    std::mutex mutex_;
    //whisper_full mutates state inside ctx_ and the segments are read back
    //out of it, so one context cannot serve two workers at once
};

}  // namespace sim
