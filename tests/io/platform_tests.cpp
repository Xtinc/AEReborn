#include "tests/support/io_fixture.h"
#include "audio_interface.h"
#include "backend/alsa_backend.h"
#include "backend/null_backend.h"
#include "backend/pull_sink.h"
#include "backend/wasapi_backend.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <cstdlib>

#if WINDOWS_OS_ENVIRONMENT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>
#include <wrl/client.h>
#endif

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;
constexpr std::size_t frames = 240;
const PortSpec pcm(rate, 1, SampleFormat::F32LE);

void exercise_device(bool capture, const DevConfig &config, DevFlags flags = DevFlags::NONE)
{
    AudioCenter audio;
    const auto device = capture ? audio.create_idev(config, flags) : audio.create_odev(config, flags);
    const auto status = AudioCenter::last_error();
    check(static_cast<bool>(device), status.what());
    NodeId monitor;
    if (capture)
    {
        const PortSpec spec(config.bandwidth, config.channels, SampleFormat::F32LE);
        monitor = audio.create_opull(spec);
        check(monitor && audio.connect(device, monitor) == RetCode::OK, "Route capture through the public graph");
    }
    const auto started = audio.start();
    check(started == RetCode::OK, started.what());
    wait_for([&] {
        const auto json = audio.dump_json();
        return json.find("\"device_status\":{\"state\":\"online\"") != std::string::npos &&
               json.find("\"steady\":true") != std::string::npos && json.find("\"drift_ppm\":") != std::string::npos;
    }, "The public endpoint exchanges frames and reports drift");
    if (capture)
    {
        std::vector<float> samples(frames * config.channels);
        wait_for([&] { return audio.pull(monitor, samples.data(), frames) > 0; }, "Public capture supplies frames");
    }
    check(audio.stop() == RetCode::OK, "Stop the public device graph");
}

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

void alsa_public_endpoints()
{
    const DevConfig virtual_pcm{"plug:null", 0, BandWidth::Full, BlockPeriod::INR_10MS, 2};
    exercise_device(true, virtual_pcm, DevFlags::RECOVERSOFT | DevFlags::MEMCPY);
    exercise_device(false, virtual_pcm);

    AudioCenter audio;
    const DevConfig missing{"AERebornNoCard"};
    check(!audio.create_idev(missing) && AudioCenter::last_error() == RetCode::DEVLOST,
          "An absent bare card fails to open without recovery");
    check(static_cast<bool>(audio.create_odev(missing, DevFlags::RECOVERSOFT)), "The absent card can wait offline");
}
#endif

#if WINDOWS_OS_ENVIRONMENT
void wasapi_rebind_after_open_error()
{
    MonitoredDuplex graph(1.0f);
    auto &input = graph.capture();
    auto &output = graph.playback();
    WasapiBackend bad_input("aereborn-missing-capture"), bad_output("aereborn-missing-render");
    NullBackend fallback_in(SampleFormat::S16LE), fallback_out(SampleFormat::S16LE);
    graph.start();
    graph.verify_block();
    expect_error(RetCode::DEVLOST, [&] { bad_input.bind(input); });
    expect_error(RetCode::DEVLOST, [&] { bad_output.bind(output); });
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

void wasapi_device_names()
{
    AudioCenter audio;
    check(!audio.create_idev({std::string(1, static_cast<char>(0xff))}, DevFlags::RECOVERSOFT) &&
              AudioCenter::last_error() == RetCode::EPARAM,
          "An invalid UTF-8 name fails before device enumeration");
    check(!audio.create_odev({"aereborn-missing-render"}) && AudioCenter::last_error() == RetCode::DEVLOST,
          "A missing friendly name cannot select the default device");
    check(!audio.create_odev({"{0.0.0.00000000}.{11223344-1234-4567-9876-0123456789AB}"}) &&
              AudioCenter::last_error() == RetCode::DEVLOST,
          "An unrecognized endpoint ID cannot select the default device");
    const auto implicit = audio.create_odev({}, DevFlags::RECOVERSOFT);
    const auto implicit_status = AudioCenter::last_error();
    const auto explicit_default = audio.create_odev({"default"}, DevFlags::RECOVERSOFT);
    check(static_cast<bool>(implicit) == static_cast<bool>(explicit_default) &&
              AudioCenter::last_error() == implicit_status,
          "Empty and literal default names use the same selection rule");
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

std::string utf8(const std::wstring &id)
{
    const auto size = WideCharToMultiByte(CP_UTF8, 0, id.c_str(), -1, nullptr, 0, nullptr, nullptr);
    check(size > 1, "The configured WASAPI endpoint ID can be encoded");
    std::string result(static_cast<std::size_t>(size), '\0');
    check(WideCharToMultiByte(CP_UTF8, 0, id.c_str(), -1, &result[0], size, nullptr, nullptr) == size,
          "Convert the configured WASAPI endpoint ID to UTF-8");
    result.pop_back();
    return result;
}

DevConfig device_config(const std::wstring &id, std::wstring &friendly)
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
    ComPtr<IPropertyStore> properties;
    check(SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf())), "The endpoint exposes properties");
    PROPVARIANT name{};
    const auto clear_name = [&]() noexcept { PropVariantClear(&name); };
    ScopeExit<decltype(clear_name)> property(clear_name);
    check(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal,
          "The endpoint exposes a friendly name");
    friendly = name.pwszVal;
    ComPtr<IAudioClient> client;
    check(SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void **>(client.GetAddressOf()))),
          "The configured endpoint provides an audio client");
    WAVEFORMATEX *raw = nullptr;
    check(SUCCEEDED(client->GetMixFormat(&raw)), "WASAPI returns the endpoint mix format");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw, CoTaskMemFree);
    const auto logical_rate = format->nSamplesPerSec == 48000 ? BandWidth::Wide : BandWidth::Full;
    return {utf8(id), 0, logical_rate, BlockPeriod::INR_10MS, format->nChannels};
}

void wasapi_device(bool input, const std::wstring &id)
{
    std::wstring friendly;
    auto config = device_config(id, friendly);
    exercise_device(input, config, DevFlags::MEMCPY);
    AudioCenter audio;
    const auto wrong_direction = input ? audio.create_odev(config) : audio.create_idev(config);
    check(!wrong_direction && AudioCenter::last_error() == RetCode::EPARAM,
          "An endpoint ID must match the requested stream direction");
    if (friendly.empty())
        return;
    const auto create_named = [&] {
        config.name = utf8(friendly);
        return input ? audio.create_idev(config) : audio.create_odev(config);
    };
    const auto named = create_named();
    check(named || AudioCenter::last_error() == RetCode::EPARAM, "A friendly name resolves or reports ambiguity");
    if (named)
        check(audio.remove(named) == RetCode::OK, "Release the endpoint resolved by name");
    if (friendly.size() > 1 && friendly.back() <= 0x7f)
    {
        friendly.pop_back();
        const auto partial = create_named();
        // A shorter name may match a different endpoint exactly, with a different channel count.
        check(partial || AudioCenter::last_error() == RetCode::EPARAM || AudioCenter::last_error() == RetCode::INVFMT,
              "A friendly-name substring reaches device selection");
    }
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
    return run_tests(argc, argv, {{"wasapi_rebind_after_open_error", wasapi_rebind_after_open_error},
                                  {"wasapi_device_names", wasapi_device_names}});
#elif defined(AEREBORN_HAS_ALSA)
    if (argc > 1 && (std::strcmp(argv[1], "alsa_capture") == 0 || std::strcmp(argv[1], "alsa_playback") == 0))
    {
        const bool capture = std::strcmp(argv[1], "alsa_capture") == 0;
        const auto *name = std::getenv(capture ? "AEREBORN_ALSA_CAPTURE_PCM" : "AEREBORN_ALSA_PLAYBACK_PCM");
        if (!name || !*name)
            return 77;
        try
        {
            DevConfig config{name};
            const auto *channels = std::getenv(capture ? "AEREBORN_ALSA_CAPTURE_CHANNELS" : "AEREBORN_ALSA_PLAYBACK_CHANNELS");
            if (channels && *channels)
            {
                std::size_t parsed = 0;
                const auto count = std::stoul(channels, &parsed);
                check(channels[parsed] == '\0' && count > 0 && count <= 1024, "Set a valid ALSA channel count");
                config.channels = count;
            }
            exercise_device(capture, config, DevFlags::MEMCPY);
            return 0;
        }
        catch (const std::exception &error)
        {
            std::cerr << "FAIL: " << argv[1] << ": " << error.what() << '\n';
            return 1;
        }
    }
    return run_tests(argc, argv, {{"alsa_interleaved", alsa_interleaved}, {"alsa_prefer_mmap", alsa_prefer_mmap},
                                  {"alsa_public_endpoints", alsa_public_endpoints}});
#endif
}
