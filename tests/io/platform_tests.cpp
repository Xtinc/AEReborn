#include "tests/support/io_fixture.h"
#include "backend/alsa_backend.h"
#include "backend/null_backend.h"
#include "backend/pull_sink.h"
#include "backend/wasapi_backend.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"

#if WINDOWS_OS_ENVIRONMENT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#endif

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;
constexpr std::size_t frames = 240;
const PortSpec pcm(rate, 1, SampleFormat::F32LE);

class MonitoredDuplex
{
  public:
    explicit MonitoredDuplex(float input_gain)
    {
        const auto gain = engine_.emplace_node<Matrix>(rate, 1, input_gain);
        const auto source = engine_.emplace_node<FloatSource>(rate, 1, signal);
        const auto mix = engine_.emplace_node<Mixer>(rate, 1, 2);
        const auto tap = engine_.emplace_node<Playback>(rate, 1, 5ms);
        connect(engine_, capture_, gain);
        connect(engine_, gain, mix);
        connect(engine_, source, mix, 1);
        connect(engine_, mix, playback_);
        connect(engine_, mix, tap);
        engine_.commit();
        monitor_.bind(*engine_.node<Playback>(tap));
    }

    Capture &capture() { return *engine_.node<Capture>(capture_); }
    Playback &playback() { return *engine_.node<Playback>(playback_); }

    void start()
    {
        monitor_.start();
        clock_.start();
    }

    void verify_block()
    {
        const auto tick = engine_.tick_position();
        clock_.advance();
        check(monitor_.pull(block_.output(0, frames)) == frames, "The monitor delivers the whole block");
        for (std::size_t i = 0; i < frames; ++i)
            check(block_.sample(i) == signal(tick * frames + i, 0), "The monitor preserves the known mix");
    }

    void reset()
    {
        monitor_.stop();
        clock_.stop();
        engine_.reset();
    }

  private:
    static float signal(std::uint64_t frame, std::size_t) { return static_cast<float>(frame % 23) / 64; }

    Engine engine_{period};
    ClockedGraph clock_{engine_};
    const NodeId capture_ = engine_.emplace_node<Capture>(rate, 1, 10ms);
    const NodeId playback_ = engine_.emplace_node<Playback>(rate, 1, 10ms);
    PullSink monitor_{pcm};
    Pcm block_{pcm, frames};
};

#if defined(AEREBORN_HAS_ALSA)
void alsa_duplex(bool prefer_mmap)
{
    MonitoredDuplex graph(0.0f);
    AlsaBackend source("plug:null", 0, prefer_mmap), sink("plug:null", 0, prefer_mmap);
    for (int pass = 0; pass < 2; ++pass)
    {
        source.bind(graph.capture());
        sink.bind(graph.playback());
        source.start();
        sink.start();
        wait_for_capture(source, QueueState::target(source));
        graph.start();
        for (std::size_t tick = 0; tick < 16; ++tick)
            graph.verify_block();
        wait_for([&] { return QueueState::primed(sink); }, "ALSA consumes its playback queue");
        check(QueueState::primed(source) && source.running() && sink.running(), "Both ALSA streams reach steady state");
        source.unbind();
        sink.unbind();
        check(source.last_error() == RetCode::OK && sink.last_error() == RetCode::OK, "Both ALSA streams cancel cleanly");
        graph.reset();
    }
}

void alsa_interleaved() { alsa_duplex(false); }
void alsa_prefer_mmap() { alsa_duplex(true); }
#endif

#if WINDOWS_OS_ENVIRONMENT
void wasapi_rebind_after_open_error()
{
    MonitoredDuplex graph(1.0f);
    auto &input = graph.capture();
    auto &output = graph.playback();
    WasapiBackend bad_input(L"aereborn-missing-capture"), bad_output(L"aereborn-missing-render");
    NullBackend fallback_in(SampleFormat::S16LE), fallback_out(SampleFormat::S16LE);
    graph.start();
    graph.verify_block();
    expect_exception<Error>([&] { bad_input.bind(input); });
    expect_exception<Error>([&] { bad_output.bind(output); });
    for (const auto *backend : {&bad_input, &bad_output})
    {
        const auto failure = backend->last_failure();
        check(!backend->bound() && failure.operation && failure.hresult < 0,
              "WASAPI retains the native error and rolls back the binding");
    }
    graph.verify_block();
    fallback_in.bind(input);
    fallback_out.bind(output);
    fallback_in.start();
    fallback_out.start();
    for (int block = 0; block < 2; ++block)
    {
        wait_for_capture(fallback_in, frames);
        graph.verify_block();
    }
    wait_for([&] { return QueueState::queued(fallback_out) == 0; }, "Null consumes the replacement playback branch");
}

std::wstring selected_device(bool capture)
{
    const auto *name = capture ? L"AEREBORN_WASAPI_CAPTURE_ID" : L"AEREBORN_WASAPI_PLAYBACK_ID";
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0)
        return {};
    std::vector<wchar_t> value(size);
    const auto written = GetEnvironmentVariableW(name, value.data(), size);
    check(written && written < size, "The configured WASAPI endpoint ID is readable");
    return {value.data(), written};
}

std::size_t device_channels(const std::wstring &id)
{
    using Microsoft::WRL::ComPtr;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "COM is available for the WASAPI probe");
    const auto cleanup = [&]() noexcept {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    };
    ScopeExit<decltype(cleanup)> com(cleanup);
    ComPtr<IMMDeviceEnumerator> devices;
    check(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(devices.GetAddressOf()))),
          "WASAPI enumerates endpoints");
    ComPtr<IMMDevice> device;
    check(SUCCEEDED(devices->GetDevice(id.c_str(), device.GetAddressOf())), "The configured endpoint exists");
    ComPtr<IAudioClient> client;
    check(SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void **>(client.GetAddressOf()))),
          "The configured endpoint provides an audio client");
    WAVEFORMATEX *raw = nullptr;
    check(SUCCEEDED(client->GetMixFormat(&raw)), "WASAPI returns the endpoint mix format");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw, CoTaskMemFree);
    return format->nChannels;
}

void wasapi_device(bool input, const std::wstring &id)
{
    const auto channels = device_channels(id);
    Engine engine(period);
    ClockedGraph clock(engine);
    WasapiBackend backend(id);
    if (input)
    {
        const auto capture = engine.emplace_node<Capture>(rate, channels, 10ms);
        const auto tap = engine.emplace_node<PacketSink>(PortSpec(rate, channels, SampleFormat::F32P));
        connect(engine, capture, tap);
        engine.commit();
        backend.bind(*engine.node<Capture>(capture));
    }
    else
    {
        const auto sink = engine.emplace_node<Playback>(rate, channels, 10ms);
        engine.commit(); // An unconnected Playback consumes graph silence.
        backend.bind(*engine.node<Playback>(sink));
    }
    backend.start();
    if (input)
        wait_for_capture(backend, QueueState::target(backend));
    clock.start();
    for (int block = 0; block < 20; ++block)
        clock.advance();
    wait_for([&] { return QueueState::primed(backend); }, "The configured WASAPI device exchanges graph frames");
    check(backend.running(), "WASAPI stays online during graph processing");
    backend.stop();
    check(backend.last_error() == RetCode::OK, "WASAPI cancellation stops the device cleanly");
}
#endif
} // namespace

int main(int argc, char **argv)
{
#if WINDOWS_OS_ENVIRONMENT
    if (argc > 1)
    {
        const std::string name(argv[1]);
        if (name == "wasapi_capture" || name == "wasapi_playback")
        {
            const bool capture = name == "wasapi_capture";
            try
            {
                const auto device = selected_device(capture);
                if (device.empty())
                {
                    std::cout << "SKIP: configure the WASAPI " << (capture ? "capture" : "playback") << " endpoint ID\n";
                    return 77;
                }
                wasapi_device(capture, device);
                return 0;
            }
            catch (const std::exception &error)
            {
                std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
                return 1;
            }
        }
    }
    return aereborn::test::run_tests(argc, argv, {{"wasapi_rebind_after_open_error", wasapi_rebind_after_open_error}});
#elif defined(AEREBORN_HAS_ALSA)
    return aereborn::test::run_tests(argc, argv, {{"alsa_interleaved", alsa_interleaved}, {"alsa_prefer_mmap", alsa_prefer_mmap}});
#endif
}
