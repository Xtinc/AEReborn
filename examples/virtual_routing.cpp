#include "core/engine.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void checked(aereborn::RetCode code)
{
    if (!code)
    {
        throw std::runtime_error(code.message());
    }
}

template <class T> T checked(aereborn::Result<T> result)
{
    checked(result.ret_code());
    return std::move(result).value();
}
} // namespace

int main()
{
    try
    {
        using namespace aereborn;
        constexpr auto rate = SampleRate::Hz48000;
        constexpr auto period = BlockPeriod::Ms05;
        // A simulated capture device delivers 441 stereo S16LE frames every 10 ms.
        const PCMSpec capture_spec{SampleRate::Hz44100, PCMFormat::S16LE, 2, 2};
        const PCMSpec playback_spec{rate, PCMFormat::S16LE, 2, 2};
        VirtualSource capture(capture_spec, period, 997.0, 0.25f);
        VirtualSink playback(playback_spec, period);
        checked(capture.prepare());
        checked(playback.prepare());

        Engine engine(rate, period);
        constexpr std::size_t channels = 2; // Logical layout stays fixed when the device format changes.
        const auto input_id = checked(engine.emplace_node<IAdapter>(channels, engine.spec()));
        const auto output_id = checked(engine.emplace_node<OAdapter>(channels, engine.spec()));
        const auto connection = checked(engine.connect({input_id}, {output_id}));
        checked(engine.prepare());
        auto *input = checked(engine.node<IAdapter>(input_id));
        auto *output = checked(engine.node<OAdapter>(output_id));
        checked(input->configure_pcm(capture_spec));
        checked(output->configure_pcm(playback_spec));
        std::vector<std::uint8_t> output_pcm(output->pcm_bytes());

        for (std::size_t transfer = 0; transfer < 4; ++transfer)
        {
            if (transfer == 2)
            {
                checked(engine.set_gain(connection, 0.5f));
            }
            checked(capture.generate());
            checked(input->bind_pcm(capture.pcm()));
            checked(output->bind_pcm({output_pcm.data(), output_pcm.size()}));
            // The adapters bridge the device's 10 ms transfer and the engine's 5 ms blocks.
            for (std::size_t step = 0; step < capture_spec.periods; ++step)
            {
                checked(engine.process());
            }
            checked(playback.receive({output_pcm.data(), output_pcm.size()}));
            std::cout << "Transfer " << transfer << ": capture=" << capture.pcm_frames()
                      << " frames, playback=" << output->pcm_frames() << " frames, received=" << playback.pcm().size()
                      << " bytes, engine position=" << engine.frame_position() << '\n';
        }
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
