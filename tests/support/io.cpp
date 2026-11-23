#include "io.h"
#include <cstring>
namespace aereborn
{
namespace test
{
Capture::Capture(PortSpec spec) : NodeT({}, {spec})
{
    if (!spec.valid())
        throw Error(ErrorCode::InvalidArg, "Invalid capture port specification");
}
void Capture::prepare(BlockPeriod period) noexcept
{
    buffer_.prepare(period);
}
void Capture::bind(View<const std::uint8_t> bytes, std::uint64_t tick)
{
    buffer_.bind(outputs()[0], bytes, tick);
}
void Capture::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].byte_view();
    const auto bytes = buffer_.take(context.tick_position);
    if (output.empty())
        return;
    if (bytes.empty())
        std::memset(output.data(), 0, output.size());
    else
        std::memcpy(output.data(), bytes.data(), output.size());
}
void Capture::reset() noexcept
{
    buffer_.reset();
}
Playback::Playback(PortSpec spec) : NodeT({spec}, {})
{
    if (!spec.valid())
        throw Error(ErrorCode::InvalidArg, "Invalid playback port specification");
}
void Playback::prepare(BlockPeriod period) noexcept
{
    buffer_.prepare(period);
}
void Playback::bind(View<std::uint8_t> bytes, std::uint64_t tick)
{
    buffer_.bind(inputs()[0], bytes, tick);
}
void Playback::process(const ProcessContext &context) noexcept
{
    const auto input = context.inputs[0].byte_view();
    const auto bytes = buffer_.take(context.tick_position);
    if (!bytes.empty() && !input.empty())
        std::memcpy(bytes.data(), input.data(), input.size());
}
void Playback::reset() noexcept
{
    buffer_.reset();
}
} // namespace test
} // namespace aereborn
