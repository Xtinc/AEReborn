#include "usr_capture.h"

#include <cstring>
#include <utility>

namespace aereborn
{

UsrCapture::UsrCapture(PortSpec spec, Callback callback)
    : ActiveBackend(false), spec_(spec), callback_(std::move(callback))
{
    if (!valid_pcm_spec(spec_) || !callback_)
        throw Error(RetCode::EPARAM, "UsrCapture requires valid interleaved PCM and a callback");
}

Result<PortSpec> UsrCapture::on_open(bool capture, PortSpec, std::chrono::microseconds target_period)
{
    if (!capture)
        return RetCode::EPARAM;
    const auto frames = frames_in_period(spec_.bandwidth, target_period);
    if (!frames)
        return frames.ret_code();
    set_device_max_frames(frames.value());
    return spec_;
}

Result<std::size_t> UsrCapture::on_read(OBBlock output) noexcept
{
    const auto bytes = output.byte_view();
    std::memset(bytes.data(), 0, bytes.size());
    callback_(output);
    return output.frames();
}

} // namespace aereborn
