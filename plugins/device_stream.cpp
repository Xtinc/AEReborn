#include "device_stream.h"
#include <cstring>

namespace aereborn
{

RetCode DeviceStreamBuffers::prepare(PortSpec input, PortSpec output, std::size_t max_frames,
                                    std::size_t queue_target, std::chrono::microseconds budget)
{
    if (!queue_target)
        return ErrorCode::NotPrepared;
    if (!max_frames)
        return ErrorCode::InvalidArg;

    const bool capture = output.format == SampleFormat::F32P;
    if (capture == (input.format == SampleFormat::F32P))
        return ErrorCode::InvalidArg;
    const auto logical = capture ? output : input;
    const auto device = capture ? input : output;
    const auto max_offset = DriftController::max_correction();
    const auto bound = capture ? Convert::max_output_frames(device, logical, max_frames, max_offset)
                               : Convert::max_input_frames(logical, device, max_frames, max_offset);
    if (!bound)
        return bound.ret_code();
    const auto logical_frames = bound.value();
    constexpr std::uint64_t us_per_second = 1000000;
    if (logical_frames > queue_target || max_frames > std::numeric_limits<std::uint64_t>::max() / us_per_second)
        return ErrorCode::TooLarge;
    const auto packet_units = max_frames * us_per_second;
    const auto native_rate = static_cast<std::uint32_t>(device.sample_rate);
    const auto packet_us = packet_units / native_rate + (packet_units % native_rate != 0);
    if (packet_us > static_cast<std::uint64_t>(budget.count()))
        return ErrorCode::TooLarge;

    const auto prepare_convert = [&](Convert &converter) {
        return capture ? converter.prepare(max_frames, logical_frames, max_offset)
                       : converter.prepare(logical_frames, max_frames, max_offset);
    };
    if (device == device_ && max_frames <= device_capacity_)
    {
        const auto code = prepare_convert(*convert);
        if (!code)
            return code;
        logical_frames_ = logical_frames;
        clock = DriftController{};
        return {};
    }

    DeviceStreamBuffers next;
    const auto allocated = next.scratch_.resize(logical.channels, logical_frames);
    if (!allocated)
        return allocated;
    next.convert = std::make_unique<Convert>(input, output);
    const auto code = prepare_convert(*next.convert);
    if (!code)
        return code;

    next.logical_ = logical;
    next.device_ = device;
    next.device_capacity_ = max_frames;
    next.logical_frames_ = logical_frames;
    *this = std::move(next);
    return {};
}

OFBlock DeviceStreamBuffers::logical_block() noexcept
{
    return scratch_.block(logical_frames_);
}

void DeviceStreamBuffers::reset() noexcept
{
    if (convert)
        convert->reset();
    clock = DriftController{};
}

void DeviceStreamBuffers::update(QueueStatus before, QueueStatus after, std::size_t device_frames) noexcept
{
    clock.update(before.queued_frames, after.queued_frames, before.target_frames, logical_.sample_rate,
                 device_frames, device_.sample_rate);
}

RetCode CaptureStream::prepare(PortSpec device, std::size_t max_frames)
{
    return buffers_.prepare(device, node_.outputs()[0], max_frames, node_.status().target_frames,
                            node_.device_read_period());
}

Result<std::size_t> CaptureStream::transfer(IBBlock input) noexcept
{
    if (!buffers_.convert)
        return ErrorCode::NotPrepared;

    const auto active = input.frames() && node_.online();
    const auto steady = active && node_.steady();
    const auto before = steady ? node_.status() : QueueStatus{};
    const auto logical = buffers_.logical_block();
    // An empty destination still validates the input and leaves the converter phase and history alone.
    const auto result = buffers_.convert->process(input, active ? logical : logical.subblock(0, 0), buffers_.rate_scale());
    if (!result)
        return result.ret_code();
    if (!active)
        return std::size_t{0};
    assert(result.value().consumed == input.frames());
    const auto written = node_.write(logical.subblock(0, result.value().produced));
    if (written && steady)
        buffers_.update(before, node_.status(), input.frames());
    return written;
}

RetCode PlaybackStream::prepare(PortSpec device, std::size_t max_frames)
{
    return buffers_.prepare(node_.inputs()[0], device, max_frames, node_.status().target_frames,
                            node_.device_write_period());
}

Result<std::size_t> PlaybackStream::transfer(OBBlock output) noexcept
{
    if (!buffers_.convert)
        return ErrorCode::NotPrepared;
    if (output.spec() != buffers_.convert->outputs()[0])
        return ErrorCode::FormatMismatch;

    auto scale = buffers_.rate_scale();
    auto needed = buffers_.convert->required_input(output.frames(), scale);
    if (!needed)
        return needed.ret_code();
    // Validate the device buffer before consuming any logical frames.
    const auto bytes = output.byte_view();
    if (output.frames() && !bytes.data())
        return ErrorCode::InvalidArg;
    if (!output.frames())
        return std::size_t{0};
    if (!node_.online())
    {
        std::memset(bytes.data(), 0, bytes.size());
        return std::size_t{0};
    }

    const auto steady = node_.steady();
    if (!steady && !needed.value())
    {
        // A stale history-only phase must not prevent a new prefill from reading the queue.
        buffers_.reset();
        scale = buffers_.rate_scale();
        needed = buffers_.convert->required_input(output.frames(), scale);
        if (!needed)
            return needed.ret_code();
    }
    const auto before = steady ? node_.status() : QueueStatus{};
    const auto logical = buffers_.logical_block();
    const auto requested = needed.value();
    const auto read = requested ? node_.read(logical.subblock(0, requested)) : Result<std::size_t>{std::size_t{0}};
    if (!read)
        return read.ret_code();
    const auto count = read.value();

    std::size_t produced = 0;
    if (requested ? count != 0 : steady)
    {
        const auto result = buffers_.convert->process(logical.subblock(0, count), output, scale);
        if (!result)
            return result.ret_code();
        assert(result.value().consumed == count);
        produced = result.value().produced;
    }
    if (produced < output.frames())
    {
        const auto offset = produced * output.spec().channels * sample_bytes(output.spec().format);
        std::memset(bytes.data() + offset, 0, bytes.size() - offset);
    }
    if (steady)
        buffers_.update(before, node_.status(), output.frames());
    return produced;
}

} // namespace aereborn
