#include "sim/audio_encode.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <FLAC/stream_encoder.h>

namespace sim {

namespace {

constexpr int kBitsPerSample = 16;
constexpr int kChannels = 1;
//the capture path is mono 16-bit throughout: Session stores int16 samples and
//the browser sends one channel. Both encoders below assume it

void append_u32_le(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void append_u16_le(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

// libFLAC hands finished frames here rather than writing a file, so the
// encoded stream accumulates in memory and never touches the disk.
FLAC__StreamEncoderWriteStatus flac_write(const FLAC__StreamEncoder*,
                                          const FLAC__byte buffer[],
                                          std::size_t bytes, std::uint32_t,
                                          std::uint32_t, void* client_data) {
    auto* out = static_cast<std::string*>(client_data);
    out->append(reinterpret_cast<const char*>(buffer), bytes);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

// RAII around the C encoder: every failure path below throws, and the handle
// has to be deleted on each one.
class FlacEncoder {
public:
    FlacEncoder() : encoder_(FLAC__stream_encoder_new()) {
        if (encoder_ == nullptr) {
            throw std::runtime_error("FLAC encoder could not be allocated");
        }
    }
    ~FlacEncoder() { FLAC__stream_encoder_delete(encoder_); }

    FlacEncoder(const FlacEncoder&) = delete;
    FlacEncoder& operator=(const FlacEncoder&) = delete;

    FLAC__StreamEncoder* get() { return encoder_; }

private:
    FLAC__StreamEncoder* encoder_;
};

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

EncodedAudio encode_wav(const std::vector<std::int16_t>& samples,
                        int sample_rate) {
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t byte_rate = static_cast<std::uint32_t>(
        sample_rate * kChannels * (kBitsPerSample / 8));

    std::string out;
    out.reserve(44 + data_bytes);

    out.append("RIFF");
    append_u32_le(out, 36 + data_bytes);  //everything after this field
    out.append("WAVE");

    out.append("fmt ");
    append_u32_le(out, 16);  //PCM fmt chunk size
    append_u16_le(out, 1);   //PCM, uncompressed
    append_u16_le(out, static_cast<std::uint16_t>(kChannels));
    append_u32_le(out, static_cast<std::uint32_t>(sample_rate));
    append_u32_le(out, byte_rate);
    append_u16_le(out, static_cast<std::uint16_t>(kChannels *
                                                  (kBitsPerSample / 8)));
    append_u16_le(out, static_cast<std::uint16_t>(kBitsPerSample));

    out.append("data");
    append_u32_le(out, data_bytes);
    out.append(reinterpret_cast<const char*>(samples.data()), data_bytes);
    //little-endian int16 is both what the samples already are in memory on
    //every platform this runs on and what the header above just promised

    return {std::move(out), "audio/wav"};
}

EncodedAudio encode_flac(const std::vector<std::int16_t>& samples,
                         int sample_rate) {
    std::string out;
    FlacEncoder encoder;

    FLAC__stream_encoder_set_channels(encoder.get(), kChannels);
    FLAC__stream_encoder_set_bits_per_sample(encoder.get(), kBitsPerSample);
    FLAC__stream_encoder_set_sample_rate(
        encoder.get(), static_cast<std::uint32_t>(sample_rate));
    FLAC__stream_encoder_set_total_samples_estimate(encoder.get(),
                                                    samples.size());
    FLAC__stream_encoder_set_compression_level(encoder.get(), 5);
    //5 is libFLAC's own default: past it the extra CPU per turn buys very
    //little on speech, and this runs while the student is waiting

    if (FLAC__stream_encoder_init_stream(encoder.get(), flac_write, nullptr,
                                         nullptr, nullptr, &out) !=
        FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        throw std::runtime_error("FLAC encoder could not be initialised");
    }

    // libFLAC takes 32-bit samples even at 16-bit depth, so the buffer has to
    // be widened rather than handed over as it stands.
    std::vector<FLAC__int32> widened(samples.begin(), samples.end());
    const FLAC__int32* channel = widened.data();

    if (!FLAC__stream_encoder_process(
            encoder.get(), &channel,
            static_cast<std::uint32_t>(widened.size()))) {
        throw std::runtime_error("FLAC encoding failed part way through");
    }

    if (!FLAC__stream_encoder_finish(encoder.get())) {
        throw std::runtime_error("FLAC encoding could not be finished");
        //the last frames and the stream MD5 are written here, so a stream that
        //skips this is truncated rather than merely unflushed
    }

    return {std::move(out), "audio/flac"};
}

EncodedAudio encode_audio(const std::vector<std::int16_t>& samples,
                          int sample_rate, AudioCodec codec) {
    return codec == AudioCodec::Wav ? encode_wav(samples, sample_rate)
                                    : encode_flac(samples, sample_rate);
}

std::string base64_encode(const std::string& bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t triple =
            (static_cast<std::uint8_t>(bytes[i]) << 16) |
            (static_cast<std::uint8_t>(bytes[i + 1]) << 8) |
            static_cast<std::uint8_t>(bytes[i + 2]);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
        out.push_back(kBase64Alphabet[triple & 0x3F]);
    }

    if (i < bytes.size()) {
        const bool has_two = (i + 1) < bytes.size();
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(
                 static_cast<std::uint8_t>(bytes[i])) << 16) |
            (has_two ? (static_cast<std::uint32_t>(
                            static_cast<std::uint8_t>(bytes[i + 1])) << 8)
                     : 0u);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(has_two ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    //the tail is the only place this can go wrong, and a mangled one corrupts
    //the last frames rather than failing, so it is spelled out rather than
    //folded into the loop above

    return out;
}

}  // namespace sim
