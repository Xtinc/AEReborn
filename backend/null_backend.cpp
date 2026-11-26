#include "null_backend.h"

#include <cstring>

namespace aereborn
{

NullBackend::NullBackend(SampleFormat format) : ActiveBackend(false), format_(format)
{
    if (!is_interleaved_pcm(format_))
        throw Error(RetCode::EPARAM);
}

Result<PortSpec> NullBackend::on_open(bool, PortSpec logical, std::chrono::microseconds period)
{
    const auto count = frames_in_period(logical.bandwidth, period);
    if (!count)
        return count.ret_code();
    logical.format = format_;
    set_device_max_frames(count.value());
    return logical;
}

Result<std::size_t> NullBackend::on_read(OBBlock output) noexcept
{
    const auto bytes = output.byte_view();
    std::memset(bytes.data(), 0, bytes.size());
    return output.frames();
}

RetCode NullBackend::on_write(IBBlock) noexcept
{
    return {};
}

} // namespace aereborn
