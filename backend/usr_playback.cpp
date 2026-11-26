#include "usr_playback.h"

#include <utility>

namespace aereborn
{

UsrPlayback::UsrPlayback(PortSpec spec, Callback callback)
    : ActiveBackend(false), spec_(spec), callback_(std::move(callback))
{
    if (!valid_pcm_spec(spec_) || !callback_)
        throw Error(RetCode::EPARAM, "UsrPlayback requires valid interleaved PCM and a callback");
}

Result<PortSpec> UsrPlayback::on_open(bool capture, PortSpec, std::chrono::microseconds target_period)
{
    if (capture)
        return RetCode::EPARAM;
    const auto frames = frames_in_period(spec_.bandwidth, target_period);
    if (!frames)
        return frames.ret_code();
    set_device_max_frames(frames.value());
    return spec_;
}

RetCode UsrPlayback::on_write(IBBlock input) noexcept
{
    callback_(input);
    return {};
}

} // namespace aereborn
