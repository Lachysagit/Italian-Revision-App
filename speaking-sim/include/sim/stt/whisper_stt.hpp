#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sim/stt.hpp"

struct whisper_context;
//forward declared rather than including whisper.h. whisper_context is an opaque
//type in whisper.h, so a pointer to it needs no definition here, and every
//translation unit that includes this header (main.cpp, server.cpp) stays free of
//the whisper headers. Declaring it outside extern "C" is fine: language linkage
//applies to function and variable names, never to class types, so this names the
//same type whisper.h does

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
    //ctx_ is a raw owning pointer, so a copy would free the same context twice.
    //A move would leave the source holding a dangling pointer unless it nulled
    //it, and mutex_ cannot be moved anyway. All four are deleted explicitly so
    //an attempt fails at the call site with a clear message rather than being
    //silently suppressed by the user-declared destructor

    std::string transcribe(const std::vector<std::int16_t>& pcm) override;
    //override pure virtual transcribe method of base class

private:
    std::string model_path_;

    whisper_context* ctx_ = nullptr;
    //loaded once in the constructor and freed once in the destructor. Null when
    //no model path was configured, which disables transcription rather than
    //failing every turn

    std::mutex mutex_;
    //whisper_full mutates state held inside ctx_, and the segment results are
    //read back out of that same state, so one context cannot serve two worker
    //threads at once. Held across the call and the result read
};

}  // namespace sim
