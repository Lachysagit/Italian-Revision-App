#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/config.hpp"

namespace sim {

// Encoded audio plus the mime type to declare for it. The two travel together
// because getting them out of step is the failure this type exists to prevent:
// audio decoded as the wrong format does not error, it transcribes into
// plausible nonsense, and nothing downstream can tell that from a bad answer.
struct EncodedAudio {
    std::string bytes;
    std::string mime_type;
};

// Both encoders are lossless and carry the same samples, so the choice cannot
// change what the model hears - only how many bytes reach it.

// The samples with a 44-byte RIFF header in front. No encoder, no dependency,
// and self-describing: the sample rate travels inside the payload rather than
// in a mime parameter that can drift out of step with the buffer.
EncodedAudio encode_wav(const std::vector<std::int16_t>& samples,
                        int sample_rate);

// Roughly half the bytes for a few ms of CPU. Worth it when this server's
// uplink is the slow part of a turn, which is a question for the "turn timings"
// log rather than for taste.
EncodedAudio encode_flac(const std::vector<std::int16_t>& samples,
                         int sample_rate);

// Dispatches on the configured codec.
EncodedAudio encode_audio(const std::vector<std::int16_t>& samples,
                          int sample_rate, AudioCodec codec);

// Base64, for the inlineData part of a Gemini request. Costs a third more
// bytes on the wire, which is the price of JSON transport.
std::string base64_encode(const std::string& bytes);

}  // namespace sim
