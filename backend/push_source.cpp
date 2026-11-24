#include "push_source.h"

#include <algorithm>

namespace aereborn
{

PushSource::PushSource(PortSpec input_spec) : input_spec_(input_spec)
{
    if (!valid_pcm_spec(input_spec_))
        throw Error(ErrorCode::InvalidArg, "PushSource requires a valid interleaved PCM PortSpec");
}

void PushSource::bind(Capture &endpoint)
{
    bind_pcm(endpoint, input_spec_);
}

std::size_t PushSource::push(IBBlock input)
{
    if (input.spec() != input_spec_)
        throw Error(ErrorCode::FormatMismatch, "PushSource input spec mismatch");
    const auto total_frames = input.frames();
    if (!total_frames)
        return 0;
    const auto *bytes = input.byte_view().data();
    if (!bytes)
        throw Error(ErrorCode::InvalidArg, "PushSource input data is null");
    if (!running())
        return 0;

    const auto frame_bytes = input_spec_.channels * sample_bytes(input_spec_.format);
    const auto max_frames = device_max_frames();
    const auto fir = needs_fir();
    // FIR output depends on stream phase. Reserve room for the entire prepared output bound.
    const auto min_free_frames = fir ? logical_frames() : std::size_t{1};

    std::size_t accepted = 0;
    while (accepted < total_frames)
    {
        const auto free_frames = capture_free_frames();
        if (free_frames < min_free_frames)
            break;
        const auto limit = std::min(total_frames - accepted, max_frames);
        const auto frames = fir ? limit : std::min(limit, free_frames);
        decode_to_capture({bytes + accepted * frame_bytes, input_spec_, frames}, 1.0);
        accepted += frames;
    }
    return accepted;
}

} // namespace aereborn
