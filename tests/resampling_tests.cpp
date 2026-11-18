#include "checked.h"
#include "pcm.h"
#include "core/engine.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"

#include <cmath>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
using TestEngine = Engine<rate, period>;
using Input = IAdapter<rate, period>;
using Output = OAdapter<rate, period>;
constexpr auto block_frames = TestEngine::block_frames();
constexpr double two_pi = 6.283185307179586;
template <SampleRate EngineRate, BlockPeriod EnginePeriod = period>
std::vector<float> input_resample(SampleRate source_rate, std::size_t pcm_frames, std::size_t periods_per_transfer)
{
    using InputType = IAdapter<EngineRate, EnginePeriod>;
    using OutputType = OAdapter<EngineRate, EnginePeriod>;
    using EngineType = Engine<EngineRate, EnginePeriod>;
    EngineType engine;
    constexpr auto graph_frames = EngineType::block_frames();
    const auto source_hz = static_cast<std::uint32_t>(source_rate);
    const PCMSpec spec{source_rate, PCMFormat::F32LE, 2, periods_per_transfer};
    const auto input_id = checked(engine.template emplace_node<InputType>(spec.channels));
    checked(checked(engine.template node<InputType>(input_id))->configure_pcm(spec));
    const auto output_id = checked(engine.template emplace_node<OutputType>(2));
    checked(checked(engine.template node<OutputType>(output_id))->configure_pcm(PCMSpec{EngineRate, PCMFormat::F32LE, 2}));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());
    auto *input = checked(engine.template node<InputType>(input_id));
    auto *output = checked(engine.template node<OutputType>(output_id));
    check(input->pcm_frames() == pcm_frames, "Input requests a fixed integer-period transfer");
    std::vector<std::uint8_t> pcm(pcm_frames * 8);
    std::vector<std::uint8_t> captured(graph_frames * 8);
    std::vector<float> converted;
    const auto bind_output = [&] { checked(output->bind_pcm({captured.data(), captured.size()})); };
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        for (std::size_t i = 0; i < pcm_frames; ++i)
        {
            const auto sample = std::sin(two_pi * 1000 * (transfer * pcm_frames + i) / source_hz);
            write_f32le(pcm.data() + 8 * i, static_cast<float>(sample));
            write_f32le(pcm.data() + 8 * i + 4, static_cast<float>(-0.5 * sample));
        }
        checked(input->bind_pcm({pcm.data(), pcm.size()}));
        for (std::size_t step = 0; step < periods_per_transfer; ++step)
        {
            bind_output();
            checked(engine.process());
            for (std::size_t i = 0; i < graph_frames; ++i)
            {
                const auto left = read_f32le(captured.data() + 8 * i);
                const auto right = read_f32le(captured.data() + 8 * i + 4);
                check(std::abs(right + 0.5f * left) < 1e-6f, "Resampling keeps separate history for each channel");
                converted.push_back(left);
            }
            if (transfer == 0 && step == 0 && periods_per_transfer > 1)
            {
                const auto first = captured;
                engine.reset();
                checked(input->bind_pcm({pcm.data(), pcm.size()}));
                bind_output();
                checked(engine.process());
                check(captured == first, "Reset clears fractional phase and filter history during a device transfer");
            }
            if (step + 1 < periods_per_transfer)
            {
                check(input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull,
                      "Input stays bound until every engine period has been consumed");
            }
        }
    }
    check(engine.frame_position() == converted.size(), "Input transfers preserve the graph period");
    engine.reset();
    bind_output();
    checked(engine.process());
    check(std::all_of(captured.begin(), captured.end(), [](std::uint8_t byte) { return byte == 0; }),
          "Reset discards pending input and filter history");
    return converted;
}

template <SampleRate EngineRate = rate, BlockPeriod EnginePeriod = period>
std::vector<float> output_resample(SampleRate target_rate, double frequency, std::size_t pcm_frames,
                                   std::size_t periods_per_transfer)
{
    using InputType = IAdapter<EngineRate, EnginePeriod>;
    using OutputType = OAdapter<EngineRate, EnginePeriod>;
    using EngineType = Engine<EngineRate, EnginePeriod>;
    const PCMSpec source_spec{EngineRate, PCMFormat::F32LE, 1};
    VirtualSource source(source_spec, EnginePeriod, frequency, 1.0f);
    checked(source.prepare());
    EngineType engine;
    const auto input_id = checked(engine.template emplace_node<InputType>(source_spec.channels));
    checked(checked(engine.template node<InputType>(input_id))->configure_pcm(source_spec));
    const auto output_id = checked(engine.template emplace_node<OutputType>(1));
    checked(checked(engine.template node<OutputType>(output_id))->configure_pcm(PCMSpec{target_rate, PCMFormat::F32LE, 1, periods_per_transfer}));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());
    auto *input = checked(engine.template node<InputType>(input_id));
    auto *output = checked(engine.template node<OutputType>(output_id));
    check(output->pcm_frames() == pcm_frames, "Output advertises the configured device frame count");
    std::vector<float> converted;
    std::vector<std::uint8_t> pcm(4 * pcm_frames);
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        checked(output->bind_pcm({pcm.data(), pcm.size()}));
        for (std::size_t step = 0; step < periods_per_transfer; ++step)
        {
            checked(source.generate());
            checked(input->bind_pcm(source.pcm()));
            checked(engine.process());
            if (step + 1 < periods_per_transfer)
            {
                check(output->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull,
                      "Output remains bound until the device period ends");
            }
        }
        for (std::size_t i = 0; i < pcm_frames; ++i)
        {
            converted.push_back(read_f32le(pcm.data() + 4 * i));
        }
    }
    return converted;
}

double rms(const std::vector<float> &samples)
{
    double energy = 0;
    for (std::size_t i = 96; i < samples.size(); ++i)
    {
        energy += samples[i] * samples[i];
    }
    return std::sqrt(energy / (samples.size() - 96));
}

void check_continuity(const std::vector<float> &samples, std::uint32_t source_rate, std::uint32_t target_rate)
{
    const auto first = 96 * target_rate / source_rate + 1;
    for (std::size_t i = first; i < samples.size(); ++i)
    {
        const auto seconds = static_cast<double>(i) / target_rate - 48.0 / source_rate;
        const auto expected = std::sin(two_pi * 1000 * seconds);
        check(std::abs(samples[i] - expected) < 0.01, "Resampling stays phase-continuous after the FIR delay");
    }
}

void continuity()
{
    check_continuity(input_resample<rate>(SampleRate::Hz44100, 441, 2), 44100, 48000);
    check_continuity(output_resample(SampleRate::Hz44100, 1000, 441, 2), 48000, 44100);

    constexpr auto reverse_rate = SampleRate::Hz44100;
    constexpr auto reverse_period = BlockPeriod::Ms10;
    check_continuity(input_resample<reverse_rate, reverse_period>(rate, 480, 1), 48000, 44100);
    check_continuity(output_resample<reverse_rate, reverse_period>(rate, 1000, 480, 1), 44100, 48000);

    check_continuity(input_resample<rate>(SampleRate::Hz11025, 441, 8), 11025, 48000);
    check_continuity(input_resample<SampleRate::Hz96000>(SampleRate::Hz8000, 40, 1), 8000, 96000);
    check_continuity(input_resample<SampleRate::Hz8000>(SampleRate::Hz96000, 480, 1), 96000, 8000);
}

void antialias()
{
    const auto passband = output_resample(SampleRate::Hz16000, 1000, 80, 1);
    const auto stopband = output_resample(SampleRate::Hz16000, 12000, 80, 1);
    check(rms(passband) > 0.69 && rms(stopband) < 0.01 * rms(passband), "Downsampling must suppress aliasing");
}

} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"continuity", continuity}, {"antialias", antialias}
    });
}
