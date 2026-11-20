#include "io.h"
#include <cstring>
#include <stdexcept>
namespace aereborn
{
Capture::Capture(PortSpec spec) : NodeT({}, {{spec}})
{
    if (!spec.valid())
        throw std::invalid_argument("Invalid capture port specification");
}
RetCode Capture::prepare(BlockPeriod period)
{
    return buffer_.prepare(period);
}
RetCode Capture::bind(View<const std::uint8_t> bytes, std::uint64_t tick) noexcept
{
    return buffer_.bind(outputs()[0], bytes, tick);
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
Playback::Playback(PortSpec spec) : NodeT({{spec}}, {})
{
    if (!spec.valid())
        throw std::invalid_argument("Invalid playback port specification");
}
RetCode Playback::prepare(BlockPeriod period)
{
    return buffer_.prepare(period);
}
RetCode Playback::bind(View<std::uint8_t> bytes, std::uint64_t tick) noexcept
{
    return buffer_.bind(inputs()[0], bytes, tick);
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
} // namespace aereborn
