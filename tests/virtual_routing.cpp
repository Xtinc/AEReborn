#include "core/engine.h"
#include "plugins/nodes.h"

#include <exception>
#include <iomanip>
#include <iostream>

namespace
{
constexpr auto rate = aereborn::SampleRate::Hz16000;
constexpr auto period = aereborn::BlockPeriod::Ms05;
using Source = aereborn::BufferSourceNode<rate, period>;
using Sink = aereborn::BufferSinkNode<rate, period>;
using Mixer = aereborn::MixerNode<rate, period>;
using Gain = aereborn::GainNode<rate, period>;

void print(const char *label, aereborn::IBlock block)
{
    std::cout << label << ':';
    // Show the first eight samples of each full 80-frame period.
    for (std::size_t frame = 0; frame < std::min(block.frames(), std::size_t{8}); ++frame)
    {
        std::cout << ' ' << block.channel(0)[frame];
    }
    std::cout << '\n';
}
} // namespace

int main()
{
    try
    {
        aereborn::Engine<rate, period> engine;
        const auto voice = engine.emplace_node<Source>(1);
        const auto music = engine.emplace_node<Source>(1);
        const auto main_mix = engine.emplace_node<Mixer>(1);
        const auto main_output = engine.emplace_node<Sink>(1);
        const auto monitor_gain = engine.emplace_node<Gain>(1, 0.25f);
        const auto monitor_mix = engine.emplace_node<Mixer>(1);
        const auto monitor_output = engine.emplace_node<Sink>(1);

        engine.connect(engine.output(voice), engine.input(main_mix));
        aereborn::ConnOpts quieter_music;
        quieter_music.gain = 0.5f;
        const auto music_connection = engine.connect(engine.output(music), engine.input(main_mix), quieter_music);
        engine.connect(engine.output(main_mix), engine.input(main_output));

        // The main output is also an input to another processing path.
        engine.connect(engine.output(main_mix), engine.input(monitor_gain));
        engine.connect(engine.output(monitor_gain), engine.input(monitor_mix));
        engine.connect(engine.output(monitor_mix), engine.input(monitor_output));
        engine.prepare();

        const float voice_data[] = {1, 2, 3, 4, 5, 6, 7, 8};
        const float music_data[] = {2, 2, 2, 2, 2, 2, 2, 2};
        auto feed = [&] {
            engine.node<Source>(voice).push_interleaved(voice_data, 8);
            engine.node<Source>(music).push_interleaved(music_data, 8);
        };
        auto render = [&] {
            if (!engine.process())
            {
                throw std::runtime_error("Graph is not ready");
            }
            print("main   ", engine.node<Sink>(main_output).block());
            print("monitor", engine.node<Sink>(monitor_output).block());
        };

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Monitor processor:\n" << engine.node(monitor_gain) << "\n\n";
        std::cout << "Main mix -> main output and monitor input\n";
        feed();
        render();

        std::cout << "\nMute one connection without rebuilding the graph\n";
        engine.set_muted(music_connection, true);
        feed();
        render();

        std::cout << "\nReconnect voice directly into the monitor mix\n";
        engine.connect(engine.output(voice), engine.input(monitor_mix));
        engine.prepare();
        feed();
        render();
        std::cout << "\nRendered frames: " << engine.frame_position() << '\n';
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
