#include "audio_interface.h"
#include "tests/support/test_common.h"

#if defined(AEREBORN_HAS_ALSA)
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#endif

namespace
{

using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

const DevConfig config{"", 0, BandWidth::Full, BlockPeriod::INR_05MS, 1};

void device_parameters()
{
    AudioCenter audio;
    auto invalid = config;
    invalid.bandwidth = BandWidth::Unknown;
    check(!audio.create_idev(invalid) && AudioCenter::last_error() == RetCode::EPARAM, "Reject invalid bandwidth");
    invalid = config;
    invalid.period = static_cast<BlockPeriod>(3);
    check(!audio.create_odev(invalid) && AudioCenter::last_error() == RetCode::EPARAM, "Reject invalid period");
    invalid = config;
    invalid.channels = 0;
    check(!audio.create_idev(invalid) && AudioCenter::last_error() == RetCode::EPARAM, "Reject zero channels");
    check(!audio.create_odev(config, static_cast<DevFlags>(8)) && AudioCenter::last_error() == RetCode::EPARAM,
          "Reject unknown flags");
    check(!audio.create_idev(config, DevFlags::RECOVERSOFT | DevFlags::RECOVERHARD) &&
              AudioCenter::last_error() == RetCode::EPARAM,
          "Reject mutually exclusive recovery flags");
#if !defined(AEREBORN_HAS_ALSA) && !WINDOWS_OS_ENVIRONMENT
    check(!audio.create_odev(config, DevFlags::RECOVERHARD) && AudioCenter::last_error() == RetCode::IOFAILED,
          "A missing platform implementation cannot create a placeholder");
#endif
}

#if defined(AEREBORN_HAS_ALSA)

class VirtualPcm
{
  public:
    explicit VirtualPcm(bool available)
    {
        set_available(available);
        if (const auto *previous = std::getenv("ALSA_CONFIG_PATH"))
        {
            previous_path_ = previous;
            had_previous_path_ = true;
        }
        check(setenv("ALSA_CONFIG_PATH", path_.c_str(), 1) == 0, "Set the process-local ALSA configuration");
    }

    ~VirtualPcm()
    {
        if (had_previous_path_)
            setenv("ALSA_CONFIG_PATH", previous_path_.c_str(), 1);
        else
            unsetenv("ALSA_CONFIG_PATH");
    }

    void set_available(bool available) const
    {
        configure(available ? "pcm.default { type null }\n" : "# No default PCM\n");
    }

    void configure(const char *definition) const
    {
        std::ofstream next(replacement_);
        next << definition;
        next.close();
        check(static_cast<bool>(next) && std::rename(replacement_.c_str(), path_.c_str()) == 0,
              "Replace the ALSA configuration atomically");
    }

  private:
    TempWave files_{"device-recovery"};
    const std::string path_ = files_.path(".conf");
    const std::string replacement_ = files_.path(".next");
    std::string previous_path_;
    bool had_previous_path_ = false;
};

bool device_has_state(const AudioCenter &audio, NodeId device, const char *state, const char *error = "OK")
{
    const auto json = audio.dump_json();
    const auto node = json.find("{\"id\":" + std::to_string(device.node()) + ',');
    const auto status = std::string("\"device_status\":{\"state\":\"") + state + "\",\"error\":\"" + error + '"';
    return json.find(status, node) < json.find('\n', node);
}

void device_offline_recovery()
{
    VirtualPcm pcm(false);
    AudioCenter audio(BlockPeriod::INR_05MS);
    check(!audio.create_idev(config, DevFlags::MEMCPY) && AudioCenter::last_error() == RetCode::DEVLOST,
          "An unavailable PCM fails immediately without recovery");
    const auto input = audio.create_idev(config, DevFlags::RECOVERSOFT | DevFlags::MEMCPY);
    const auto output = audio.create_odev(config, DevFlags::RECOVERSOFT | DevFlags::MEMCPY);
    const auto monitor = audio.create_opull({BandWidth::Full, 1, SampleFormat::F32LE}, 5ms);
    check(input && output && monitor, "Create offline hardware endpoints and a monitor");
    check(audio.connect(input, output) == RetCode::OK && audio.connect(input, monitor) == RetCode::OK,
          "Connect the offline endpoints through the public graph");
    audio.push(input, nullptr, 0);
    check(AudioCenter::last_error() == RetCode::ETYPE, "Hardware capture rejects passive push");
    audio.pull(output, nullptr, 0);
    check(AudioCenter::last_error() == RetCode::ETYPE, "Hardware playback rejects passive pull");
    check(audio.start() == RetCode::OK, "Start the graph with unavailable devices");
    const auto started = std::chrono::steady_clock::now();
    check(device_has_state(audio, input, "waiting", "DEVLOST") &&
              device_has_state(audio, output, "waiting", "DEVLOST"),
          "Both offline placeholders report the bind failure");

    std::array<float, 240> samples;
    samples.fill(1.0f);
    wait_for([&] { return audio.pull(monitor, samples.data(), samples.size()) == samples.size(); },
             "The monitor receives a complete graph block");
    check(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; }),
          "Offline capture supplies silence");

    check(audio.set_paused(output, true) == RetCode::OK && audio.commit() == RetCode::OK,
          "Commit a pause while playback is waiting");
    pcm.set_available(true);
    wait_for([&] { return device_has_state(audio, input, "online"); }, "The next health check opens capture");
    check(std::chrono::steady_clock::now() - started >= 950ms, "Capture honors the one-second health interval");
    check(device_has_state(audio, output, "stopped", "DEVLOST"), "Paused playback keeps its last bind error");
    check(audio.set_paused(output, false) == RetCode::OK && audio.commit() == RetCode::OK &&
              device_has_state(audio, output, "online"),
          "Playback opens when it reenters the running plan");
}

void device_periodic_reset()
{
    VirtualPcm pcm(true);
    AudioCenter audio;
    const auto hard = audio.create_odev(config, DevFlags::RECOVERHARD | DevFlags::MEMCPY);
    const auto soft = audio.create_odev(config, DevFlags::RECOVERSOFT | DevFlags::MEMCPY);
    check(hard && soft && audio.start() == RetCode::OK, "Start strict and soft playback on the virtual PCM");
    const auto started = std::chrono::steady_clock::now();

    // The already-open PCM stays usable. A new handle needs the current configuration.
    pcm.set_available(false);
    std::this_thread::sleep_until(started + 9s);
    check(device_has_state(audio, hard, "online"), "Strict playback stays online before the reset deadline");
    wait_for([&] { return device_has_state(audio, hard, "waiting", "DEVLOST"); },
             "Strict playback tries to reopen the PCM after ten seconds", 5s);
    check(std::chrono::steady_clock::now() - started >= 9500ms, "Strict playback honors the reset interval");
    check(device_has_state(audio, soft, "online"), "Soft playback keeps its original handle");

    pcm.set_available(true);
    wait_for([&] { return device_has_state(audio, hard, "online"); }, "The next health check reopens strict playback");
    check(audio.remove(hard) == RetCode::OK && audio.stop() == RetCode::OK, "Remove and stop the endpoints");
    std::this_thread::sleep_for(1100ms);
    check(device_has_state(audio, soft, "stopped") && audio.dump_json().find("\"online\":true") == std::string::npos,
          "The graph stays stopped and the PCM stays offline past the next health check");
}

void device_manual_restart()
{
    VirtualPcm pcm(true);
    auto named = config;
    named.name = "restart:playback";
    const char *faulty = "pcm.restart { @args [ NAME ] @args.NAME { type string } "
                         "type file slave.pcm { type null } file \"/dev/full\" format raw }\n";
    const char *healthy = "pcm.restart { @args [ NAME ] @args.NAME { type string } type null }\n";
    for (const auto recovery : {DevFlags::NONE, DevFlags::RECOVERSOFT, DevFlags::RECOVERHARD})
    {
        pcm.configure(faulty);
        AudioCenter audio;
        const auto output = audio.create_odev(named, recovery | DevFlags::MEMCPY);
        check(output && audio.start() == RetCode::OK, "Start a named PCM whose writes fail");
        const auto state = recovery == DevFlags::NONE ? "failed" : "waiting";
        wait_for([&] { return device_has_state(audio, output, state, "IOFAILED"); }, "The worker reports the write failure");
        check(audio.stop() == RetCode::OK && device_has_state(audio, output, "stopped", "IOFAILED"),
              "Stop retains the worker error");
        pcm.configure(healthy);
        check(audio.start() == RetCode::OK && device_has_state(audio, output, "online"),
              "Restart opens the new PCM immediately");
        std::this_thread::sleep_for(30ms);
        check(device_has_state(audio, output, "online"), "Restart keeps using the working PCM");
    }

    pcm.set_available(true);
    AudioCenter audio;
    auto defaults = config;
    defaults.name = "default";
    const auto output = audio.create_odev(defaults, DevFlags::MEMCPY);
    check(output && audio.start() == RetCode::OK && audio.stop() == RetCode::OK, "Stop a healthy default stream");
    pcm.set_available(false);
    check(audio.start() == RetCode::DEVLOST, "Restart resolves the current default instead of using the old PCM");
    pcm.set_available(true);
    check(audio.start() == RetCode::OK && device_has_state(audio, output, "online"),
          "A later restart opens the available default PCM");
}

#endif

} // namespace

int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"device_parameters", device_parameters},
#if defined(AEREBORN_HAS_ALSA)
                                 {"device_offline_recovery", device_offline_recovery},
                                 {"device_periodic_reset", device_periodic_reset},
                                 {"device_manual_restart", device_manual_restart},
#endif
                                 });
}
