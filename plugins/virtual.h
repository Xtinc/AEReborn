#ifndef AEREBORN_PLUGINS_VIRTUAL_H
#define AEREBORN_PLUGINS_VIRTUAL_H

#include "adapter.h"
#include <cmath>
#include <vector>

namespace aereborn
{

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class VirtualSource final : public NodeT<0, 1, Rate, Period>, private PCMConverter
{
  public:
    explicit VirtualSource(std::size_t channels = 1, double frequency_hz = 440.0, float amplitude = 0.25f,
                           PCMFormat format = PCMFormat::F32LE)
        : NodeT<0, 1, Rate, Period>({}, {{channels}}), PCMConverter({Rate, format, channels}, Rate, Rate, Period),
          frequency_hz_(frequency_hz), amplitude_(amplitude)
    {
    }

    // Interleaved PCM from the last processed block.
    View<const std::uint8_t> pcm() const noexcept
    {
        return {pcm_.data(), pcm_.size()};
    }

  private:
    RetCode prepare() override
    {
        if (!(frequency_hz_ >= 0.0 && frequency_hz_ < sample_rate_hz_ / 2.0) || !std::isfinite(amplitude_))
        {
            return ErrorCode::InvalidArg;
        }
        const auto code = prepare_pcm();
        if (!code)
        {
            return code;
        }
        pcm_.resize(pcm_bytes());
        return {};
    }

    void process(const ProcContext &context) noexcept override
    {
        constexpr double two_pi = 6.28318530717958647692;
        const auto phase_step = two_pi * frequency_hz_ / sample_rate_hz_;
        const auto phase = std::fmod(static_cast<double>(context.frame_position) * phase_step, two_pi);
        const auto output = context.outputs[0];
        for (std::size_t frame = 0; frame < output.frames(); ++frame)
        {
            const auto sample = amplitude_ * static_cast<float>(std::sin(phase + frame * phase_step));
            for (std::size_t ch = 0; ch < output.channels(); ++ch)
            {
                output.channel(ch)[frame] = sample;
            }
        }
        encode(output, 0, pcm_.data());
        if (spec_.format != PCMFormat::F32LE)
        {
            decode(pcm_.data(), 0, output);
        }
    }

    static constexpr auto sample_rate_hz_ = static_cast<std::uint32_t>(Rate);
    const double frequency_hz_;
    const float amplitude_;
    std::vector<std::uint8_t> pcm_;
};

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class VirtualSink final : public NodeT<1, 0, Rate, Period>, private PCMConverter
{
  public:
    explicit VirtualSink(std::size_t channels = 1, PCMFormat format = PCMFormat::F32LE)
        : NodeT<1, 0, Rate, Period>({{channels}}, {}), PCMConverter({Rate, format, channels}, Rate, Rate, Period)
    {
    }

    // Interleaved PCM from the last processed block, empty until the first block after reset.
    View<const std::uint8_t> pcm() const noexcept
    {
        return {pcm_.data(), received_ ? pcm_.size() : 0};
    }

    // Compare the decoded PCM to a planar reference using an absolute sample tolerance.
    bool matches(IBlock expected, float tolerance = 0.0f) noexcept
    {
        if (!received_ || expected.channels() != spec_.channels || expected.frames() != pcm_frames() ||
            !(tolerance >= 0.0f && std::isfinite(tolerance)))
        {
            return false;
        }
        const auto actual = samples_.block(pcm_frames());
        decode(pcm_.data(), 0, actual);
        for (std::size_t ch = 0; ch < actual.channels(); ++ch)
        {
            const auto *actual_samples = actual.channel(ch);
            const auto *expected_samples = expected.channel(ch);
            for (std::size_t frame = 0; frame < actual.frames(); ++frame)
            {
                if (!(std::abs(actual_samples[frame] - expected_samples[frame]) <= tolerance))
                {
                    return false;
                }
            }
        }
        return true;
    }

  private:
    RetCode prepare() override
    {
        const auto code = prepare_pcm();
        if (!code)
        {
            return code;
        }
        pcm_.resize(pcm_bytes());
        return samples_.resize(spec_.channels, pcm_frames());
    }

    void process(const ProcContext &context) noexcept override
    {
        encode(context.inputs[0], 0, pcm_.data());
        received_ = true;
    }

    void reset() noexcept override
    {
        received_ = false;
    }

    std::vector<std::uint8_t> pcm_;
    Buffer samples_;
    bool received_ = false;
};

} // namespace aereborn

#endif // AEREBORN_PLUGINS_VIRTUAL_H
