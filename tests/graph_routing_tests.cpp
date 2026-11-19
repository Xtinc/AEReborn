#include "support/routing_fixture.h"

namespace
{
using namespace aereborn;
using namespace aereborn::test;

void one_to_one()
{
    Engine engine(render_rate, render_period);
    Playback output(engine, 2);
    Capture input(engine, 2, 997, 0.25f);
    checked(engine.connect({input.id}, {output.id}));
    checked(engine.prepare());
    for (std::size_t tick = 0; tick < 4; ++tick)
    {
        render_route(engine, {&input}, {&output});
        check(output.device.matches(input.device.pcm()), "Direct virtual-device route preserves F32 PCM");
        output.expect([&](std::size_t frame, std::size_t) { return tone(997, 0.25, tick * render_frames + frame); });
    }
}

void processor_chain()
{
    Engine engine(render_rate, render_period);
    Playback output(engine, 1);
    // Reverse creation order must not determine the execution order.
    const auto second = checked(engine.emplace_node<VirtualNode<>>(1, -2.0f));
    const auto first = checked(engine.emplace_node<VirtualNode<>>(1, 0.5f));
    Capture input(engine, 1, 733, 0.25f);
    checked(engine.connect({second}, {output.id}));
    checked(engine.connect({first}, {second}));
    checked(engine.connect({input.id}, {first}));
    checked(engine.prepare());
    render_route(engine, {&input}, {&output});
    output.expect([](std::size_t frame, std::size_t) { return -tone(733, 0.25, frame); });
    check(checked(engine.node<VirtualNode<>>(first))->calls == 1 &&
              checked(engine.node<VirtualNode<>>(second))->calls == 1, "Each processor executes once");
}

void one_to_many()
{
    Engine engine(render_rate, render_period);
    Playback main(engine, 1);
    Playback monitor(engine, 1);
    Capture input(engine, 1, 997, 0.25f);
    const auto bus = checked(engine.emplace_node<VirtualNode<>>(1, 0.5f));
    checked(engine.connect({input.id}, {bus}));
    checked(engine.connect({bus}, {main.id}));
    checked(engine.connect({bus}, {monitor.id}, {-0.5f, false, {}}));
    checked(engine.prepare());
    for (std::size_t tick = 0; tick < 4; ++tick)
    {
        render_route(engine, {&input}, {&main, &monitor});
        main.expect([&](std::size_t frame, std::size_t) { return tone(997, 0.125, tick * render_frames + frame); });
        monitor.expect([&](std::size_t frame, std::size_t) { return -tone(997, 0.0625, tick * render_frames + frame); });
        check(checked(engine.node<VirtualNode<>>(bus))->calls == tick + 1,
              "A shared bus executes once regardless of the number of destinations");
    }
}

void many_to_one()
{
    Engine engine(render_rate, render_period);
    Playback output(engine, 1);
    Capture voice(engine, 1, 997, 0.25f);
    Capture music(engine, 1, 431, 0.125f);
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto idle = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({voice.id}, {bus}));
    checked(engine.connect({music.id}, {bus}, {0.5f, false, {}}));
    checked(engine.connect({bus}, {output.id}));
    checked(engine.prepare());
    render_route(engine, {&voice, &music}, {&output});
    output.expect([](std::size_t frame, std::size_t) { return tone(997, 0.25, frame) + tone(431, 0.0625, frame); });
    check(checked(engine.node<VirtualNode<>>(idle))->calls == 0 &&
              checked(engine.node<VirtualNode<>>(idle))->prepares == 0,
          "A node with no path to a sink is not prepared or processed");
}

void many_to_many()
{
    Engine engine(render_rate, render_period);
    Playback main(engine, 1);
    Playback monitor(engine, 1);
    Capture a(engine, 1, 997, 0.25f);
    Capture b(engine, 1, 431, 0.125f);
    const auto a_main = checked(engine.connect({a.id}, {main.id}));
    checked(engine.connect({b.id}, {main.id}, {0.5f, false, {}}));
    checked(engine.connect({a.id}, {monitor.id}, {-0.5f, false, {}}));
    checked(engine.connect({b.id}, {monitor.id}));
    checked(engine.prepare());
    for (std::size_t tick = 0; tick < 3; ++tick)
    {
        if (tick == 1)
        {
            checked(engine.set_muted(a_main, true));
        }
        if (tick == 2)
        {
            checked(engine.set_muted(a_main, false));
            checked(engine.set_gain(a_main, -1));
        }
        render_route(engine, {&a, &b}, {&main, &monitor});
        main.expect([&](std::size_t frame, std::size_t) {
            const auto index = tick * render_frames + frame;
            const auto gain = tick == 0 ? 1 : tick == 1 ? 0 : -1;
            return gain * tone(997, 0.25, index) + tone(431, 0.0625, index);
        });
        monitor.expect([&](std::size_t frame, std::size_t) {
            const auto index = tick * render_frames + frame;
            return -tone(997, 0.125, index) + tone(431, 0.125, index);
        });
    }
    check(engine.frame_position() == 3 * render_frames, "Connection parameters apply without rebuilding the graph");
}

void bus_chain_and_taps()
{
    Engine engine(render_rate, render_period);
    Playback pre_effects(engine, 1);
    Playback post_effects(engine, 1);
    Capture a(engine, 1, 997, 0.25f);
    Capture b(engine, 1, 431, 0.125f);
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto effect = checked(engine.emplace_node<VirtualNode<>>(1, 0.5f));
    const auto master = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({a.id}, {bus}));
    checked(engine.connect({b.id}, {bus}, {0.25f, false, {}}));
    checked(engine.connect({bus}, {pre_effects.id}));
    checked(engine.connect({bus}, {effect}));
    checked(engine.connect({effect}, {master}));
    checked(engine.connect({b.id}, {master}, {0.5f, false, {}}));
    checked(engine.connect({master}, {post_effects.id}));
    checked(engine.prepare());
    render_route(engine, {&a, &b}, {&pre_effects, &post_effects});
    pre_effects.expect([](std::size_t frame, std::size_t) { return tone(997, 0.25, frame) + tone(431, 0.03125, frame); });
    post_effects.expect([](std::size_t frame, std::size_t) { return tone(997, 0.125, frame) + tone(431, 0.078125, frame); });
    check(checked(engine.node<VirtualNode<>>(bus))->calls == 1 &&
              checked(engine.node<VirtualNode<>>(effect))->calls == 1 &&
              checked(engine.node<VirtualNode<>>(master))->calls == 1,
          "Bus taps reuse existing outputs without additional execution loops");
}

void ports_and_matrices()
{
    Engine engine(render_rate, render_period);
    Playback stereo(engine, 2);
    Playback mono(engine, 1);
    Capture a(engine, 1, 997, 0.25f);
    Capture b(engine, 1, 431, 0.125f);
    const auto router = checked(engine.emplace_node<VirtualNode<2, 2>>(2));
    checked(engine.connect({b.id}, {router, 1}, {1, false, {2, 1, {0.5f, 2}}}));
    checked(engine.connect({a.id}, {router, 0}, {1, false, {2, 1, {1, -1}}}));
    checked(engine.connect({router, 1}, {mono.id}, {1, false, {1, 2, {0.25f, 0.75f}}}));
    checked(engine.connect({router, 0}, {stereo.id}));
    checked(engine.prepare());
    render_route(engine, {&a, &b}, {&stereo, &mono});
    stereo.expect([](std::size_t frame, std::size_t ch) {
        return (ch == 0 ? 1 : -1) * tone(997, 0.25, frame) + (ch == 0 ? 0.5 : 2) * tone(431, 0.125, frame);
    });
    mono.expect([](std::size_t frame, std::size_t) { return -tone(997, 0.125, frame) + tone(431, 0.203125, frame); });
    check(checked(engine.node<VirtualNode<2, 2>>(router))->calls == 1, "All ports share one node execution");
}

void missing_endpoints()
{
    Engine engine(render_rate, render_period);
    Capture a(engine, 1, 997, 0.25f);
    Capture b(engine, 1, 431, 0.125f);
    Playback main(engine, 1);
    Playback monitor(engine, 1);
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({a.id}, {bus}));
    checked(engine.connect({b.id}, {bus}));
    checked(engine.connect({bus}, {main.id}));
    checked(engine.connect({bus}, {monitor.id}));
    checked(engine.prepare());
    render_route(engine, {&a}, {&main});
    main.expect([](std::size_t frame, std::size_t) { return tone(997, 0.25, frame); });
    for (std::size_t tick = 0; tick < 2; ++tick)
    {
        render_route(engine, {&b}, {&monitor});
        monitor.expect([&](std::size_t frame, std::size_t) { return tone(431, 0.125, tick * render_frames + frame); });
    }
    // Both sources are absent, so this period overwrites both outputs with silence.
    render_route(engine, {}, {&main, &monitor});
    main.expect([](std::size_t, std::size_t) { return 0; });
    monitor.expect([](std::size_t, std::size_t) { return 0; });
}

void mixed_device_cadence()
{
    Engine engine(render_rate, render_period);
    const PCMSpec capture_spec{SampleRate::Hz44100, PCMFormat::F32LE, 1, 2};
    const PCMSpec playback_spec{render_rate, PCMFormat::F32LE, 1, 3};
    VirtualSource capture(capture_spec, render_period, 997, 0.25f);
    VirtualSink playback(playback_spec, render_period);
    checked(capture.prepare());
    checked(playback.prepare());
    const auto input_id = checked(engine.emplace_node<IAdapter>(1, engine.spec()));
    const auto output_id = checked(engine.emplace_node<OAdapter>(1, engine.spec()));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    checked(input->configure_pcm(capture_spec));
    checked(output->configure_pcm(playback_spec));
    Capture native(engine, 1, 431, 0.125f);
    Playback monitor(engine, 1);
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({input_id}, {bus}));
    checked(engine.connect({native.id}, {bus}));
    checked(engine.connect({bus}, {output_id}));
    checked(engine.connect({bus}, {monitor.id}));
    checked(engine.prepare());
    check(input->pcm_frames() == 441 && output->pcm_frames() == 720,
          "Device packet sizes follow two capture periods and three playback periods");
    std::vector<std::uint8_t> packet(output->pcm_bytes());
    std::vector<std::uint8_t> observed(packet.size());
    for (std::size_t tick = 0; tick < 6; ++tick)
    {
        if (tick % 2 == 0)
        {
            checked(capture.generate());
            checked(input->bind_pcm(capture.pcm()));
        }
        if (tick % 3 == 0)
        {
            checked(output->bind_pcm({packet.data(), packet.size()}));
        }
        render_route(engine, {&native}, {&monitor});
        const auto pcm = monitor.device.pcm();
        std::copy(pcm.begin(), pcm.end(), observed.begin() + (tick % 3) * render_frames * 4);
        for (std::size_t frame = 0; frame < render_frames; ++frame)
        {
            const auto index = tick * render_frames + frame;
            if (index > 105) // Past the existing 96-tap filter's startup history.
            {
                const auto expected = 0.25 * std::sin(6.283185307179586 * 997 *
                                                     (static_cast<double>(index) / 48000 - 48.0 / 44100)) +
                                      tone(431, 0.125, index);
                check(std::abs(read_f32le(pcm.data() + frame * 4) - expected) < 0.005,
                      "Mixed-rate capture follows its own cadence while the native source stays aligned");
            }
        }
        if (tick % 3 == 2)
        {
            checked(playback.receive({packet.data(), packet.size()}));
            check(playback.matches({observed.data(), observed.size()}),
                  "Three-period playback contains the same ordered blocks as the one-period monitor");
        }
    }
    check(checked(engine.node<VirtualNode<>>(bus))->calls == 6 && engine.frame_position() == 6 * render_frames,
          "Multiple device cadences share one graph driver");
}

void topology_rebuild()
{
    Engine engine(render_rate, render_period);
    Capture input(engine, 1, 997, 0.25f);
    Playback main(engine, 1);
    const auto processor = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({input.id}, {processor}));
    checked(engine.connect({processor}, {main.id}));
    input.generate();
    main.bind();
    checked(engine.prepare());
    checked(engine.process());
    main.receive();
    main.expect([](std::size_t frame, std::size_t) { return tone(997, 0.25, frame); });
    input.generate();
    main.bind();
    const auto previous = main.bytes;
    Playback monitor(engine, 1);
    checked(engine.connect({processor}, {monitor.id}));
    expect_error(ErrorCode::NotPrepared, engine.process());
    check(main.bytes == previous && engine.frame_position() == render_frames, "Graph edits do not consume pending PCM");
    expect_error(ErrorCode::BufferFull, input.adapter->bind_pcm(input.device.pcm()));
    checked(engine.prepare());
    monitor.bind();
    checked(engine.process());
    main.receive();
    monitor.receive();
    main.expect([](std::size_t frame, std::size_t) { return tone(997, 0.25, render_frames + frame); });
    check(monitor.device.matches(main.device.pcm()), "A newly attached monitor receives the same existing output");
    auto *node = checked(engine.node<VirtualNode<>>(processor));
    check(node->calls == 2 && node->prepares == 1 && node->resets == 0, "Rebuilding keeps existing node state");
    checked(engine.remove_node(processor));
    checked(engine.prepare());
    render_route(engine, {}, {&main, &monitor});
    main.expect([](std::size_t, std::size_t) { return 0; });
    monitor.expect([](std::size_t, std::size_t) { return 0; });
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"one_to_one", one_to_one}, {"processor_chain", processor_chain},
        {"one_to_many", one_to_many}, {"many_to_one", many_to_one},
        {"many_to_many", many_to_many}, {"bus_chain_and_taps", bus_chain_and_taps},
        {"ports_and_matrices", ports_and_matrices}, {"missing_endpoints", missing_endpoints},
        {"mixed_device_cadence", mixed_device_cadence}, {"topology_rebuild", topology_rebuild}
    });
}
