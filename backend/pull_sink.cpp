#include "pull_sink.h"

#include <algorithm>

namespace aereborn
{

PullSink::PullSink(PortSpec output_spec) : output_spec_(output_spec)
{
    if (!valid_pcm_spec(output_spec_))
        throw Error(RetCode::EPARAM, "PullSink requires a valid interleaved PCM PortSpec");
}

void PullSink::bind(Playback &endpoint)
{
    bind_pcm(endpoint, output_spec_);
}

std::size_t PullSink::pull(OBBlock output)
{
    if (output.spec() != output_spec_)
        throw Error(RetCode::INVFMT, "PullSink output spec mismatch");
    const auto total_frames = output.frames();
    if (!total_frames)
        return 0;
    auto *bytes = output.byte_view().data();
    if (!bytes)
        throw Error(RetCode::EPARAM, "PullSink output data is null");
    if (!running())
        return 0;

    const auto frame_bytes = output_spec_.channels * sample_bytes(output_spec_.format);
    const auto max_frames = device_max_frames();
    std::size_t produced = 0;
    while (produced < total_frames)
    {
        const auto queued = available();
        const auto frames = std::min(total_frames - produced, max_frames);
        const auto requested = required_input(frames);
        if (queued < requested && (frames == 1 || queued < required_input(1)))
            break;

        const auto logical = logical_block(std::min(requested, queued));
        const auto received = read_playback(logical);
        const OBBlock destination(bytes + produced * frame_bytes, output_spec_, frames);
        const auto written = encode_from_playback(logical.subblock(0, received), destination, 1.0).frames();
        produced += written;
        if (written < frames)
            break;
    }
    return produced;
}

} // namespace aereborn
