#include "audio_interface.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

using namespace aereborn;

NodeId check_node(NodeId id, const char *what)
{
    if (!id)
        throw std::runtime_error(std::string(what) + " failed: " + AudioCenter::last_error().what());
    return id;
}

void check_rc(RetCode rc, const char *what)
{
    if (!rc)
        throw std::runtime_error(std::string(what) + " failed: " + rc.what());
}

void dump_text_now(const AudioCenter &audio)
{
    const std::string report = audio.dump_text();
    if (report.empty())
        throw std::runtime_error(std::string("dump_text failed: ") + AudioCenter::last_error().what());
    std::cout << "\n================ DUMP TEXT ================\n" << report << '\n';
}

void dump_dot_now(const AudioCenter &audio, const std::string &dot_path, bool print_dot)
{
    const std::string dot = audio.dump_dot();
    if (dot.empty())
        throw std::runtime_error(std::string("dump_dot failed: ") + AudioCenter::last_error().what());

    std::ofstream out(dot_path);
    if (!out)
        throw std::runtime_error("Failed to open DOT output file: " + dot_path);
    out << dot;

    if (print_dot)
        std::cout << "\n================ DUMP DOT ================\n" << dot << '\n';

    std::cout << "DOT written to: " << dot_path << "\n"
              << "Render SVG    : dot -Tsvg " << dot_path << " -o complex_public_scene.svg\n";
}

void print_help()
{
    std::cout << "\nEngine is running continuously. Commands:\n"
              << "  text         - dump_text() live state\n"
              << "  dot          - dump_dot() to complex_public_scene.dot\n"
              << "  gain <val>   - set master_gain (e.g. gain 0.5)\n"
              << "  high         - toggle high priority on main_mix[1] (voice)\n"
              << "  mute         - toggle mute on main_mix[2] (extra)\n"
              << "  pause        - toggle pause on rec_paused + live commit()\n"
              << "  q / quit     - stop engine and finalize WAV files\n";
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <bgm.wav> <voice.wav> [extra.wav]\n";
        return 1;
    }

    try
    {
        const std::string bgm_path = argv[1];
        const std::string voice_path = argv[2];
        const std::string extra_path = argc >= 4 ? argv[3] : argv[1];

        const std::string out_master_path = "scene_out_master_48k.wav";
        const std::string out_monitor_path = "scene_out_monitor_16k.wav";
        const std::string out_segment_path = "scene_out_segment_44k.wav";
        const std::string out_paused_path = "scene_out_paused_48k.wav";
        const std::string dot_path = "complex_public_scene.dot";

        // 单文件 create_owav 为独占打开，运行前清理旧输出避免返回 BUSY
        for (const auto &p : {out_master_path, out_monitor_path, out_paused_path})
            std::remove(p.c_str());

        AudioCenter audio(BlockPeriod::INR_10MS);

        // =====================================================================
        // 1. 创建节点（纯公开接口，输入自动探测 WAV 规格并循环播放）
        // =====================================================================
        const auto src_bgm = check_node(audio.create_iwav(bgm_path, -1), "create_iwav(bgm)");
        const auto src_voice = check_node(audio.create_iwav(voice_path, -1), "create_iwav(voice)");
        const auto src_extra = check_node(audio.create_iwav(extra_path, -1), "create_iwav(extra)");

        // 子总线（48kHz 2ch）+ 自定义 ChannelMap（左右互换并衰减右声道）
        const auto sub_mix = check_node(audio.create_mixer(BandWidth::Full, 2, 2), "create_mixer(sub)");
        const auto sub_map = ChannelMap::identity(2).swap_outputs(0, 1).scale_output(1, 0.85f);
        const auto sub_fx = check_node(audio.create_matrix(BandWidth::Full, sub_map, 0.90f), "create_matrix(sub_fx)");

        // 主总线（48kHz 2ch，4 路输入，slot 3 留空）
        const auto main_mix = check_node(audio.create_mixer(BandWidth::Full, 2, 4), "create_mixer(main)");
        const auto master_gain =
            check_node(audio.create_matrix(BandWidth::Full, 2, 0.60f), "create_matrix(master_gain)");

        // 显式单声道混缩矩阵
        const auto mono_down =
            check_node(audio.create_matrix(BandWidth::Full, ChannelMap::to_mono(2)), "create_matrix(mono_down)");

        // 4 路异构输出（标准录盘 / 16k 单声道监听 / 44.1k 分段录盘 / 可暂停支路）
        const auto rec_master = check_node(
            audio.create_owav(out_master_path, PortSpec(BandWidth::Full, 2, SampleFormat::S16LE)),
            "create_owav(master)");
        const auto rec_monitor = check_node(
            audio.create_owav(out_monitor_path, PortSpec(BandWidth::Wide, 1, SampleFormat::S16LE)),
            "create_owav(monitor)");
        const auto rec_segment = check_node(
            audio.create_owav(out_segment_path, PortSpec(BandWidth::CDQuality, 2, SampleFormat::F32LE),
                              std::chrono::seconds(2)),
            "create_owav(segment)");
        const auto rec_paused = check_node(
            audio.create_owav(out_paused_path, PortSpec(BandWidth::Full, 2, SampleFormat::S24_3LE)),
            "create_owav(paused)");

        // =====================================================================
        // 2. 连线（跨采样率/声道由 AudioCenter 自动插桥）
        // =====================================================================
        check_rc(audio.connect(src_bgm, sub_mix[0]), "connect(src_bgm -> sub_mix[0])");
        check_rc(audio.connect(sub_mix, sub_fx), "connect(sub_mix -> sub_fx)");
        check_rc(audio.connect(sub_fx, main_mix[0]), "connect(sub_fx -> main_mix[0])");
        check_rc(audio.connect(sub_fx, rec_paused), "connect(sub_fx -> rec_paused)");

        check_rc(audio.connect(src_voice, main_mix[1]), "connect(src_voice -> main_mix[1])");
        check_rc(audio.connect(src_extra, main_mix[2]), "connect(src_extra -> main_mix[2])");

        check_rc(audio.connect(main_mix, master_gain), "connect(main_mix -> master_gain)");
        check_rc(audio.connect(master_gain, rec_master), "connect(master_gain -> rec_master)");
        check_rc(audio.connect(master_gain, mono_down), "connect(master_gain -> mono_down)");
        check_rc(audio.connect(mono_down, rec_monitor), "connect(mono_down -> rec_monitor)");
        check_rc(audio.connect(master_gain, rec_segment), "connect(master_gain -> rec_segment)");

        // =====================================================================
        // 3. 初始混音策略与启动（引擎进入持续运行态）
        // =====================================================================
        bool voice_high = true;
        bool extra_muted = true;
        bool branch_paused = true;

        check_rc(audio.set_high_priority(main_mix[1], voice_high), "set_high_priority(voice)");
        check_rc(audio.set_muted(main_mix[2], extra_muted), "set_muted(extra)");

        check_rc(audio.start(), "start"); // Plan #1 启动

        // 稍微等几个 tick 进入稳态后，演示运行期热暂停（Plan #2），再输出首次完整诊断
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check_rc(audio.set_paused(rec_paused, branch_paused), "set_paused(rec_paused)");
        check_rc(audio.commit(), "commit(Plan #2)");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        dump_text_now(audio);
        dump_dot_now(audio, dot_path, true);
        print_help();

        // =====================================================================
        // 4. 引擎在后台持续运行，主线程按用户输入实时控制与诊断
        // =====================================================================
        std::string line;
        while (std::cout << "\naereborn> " << std::flush && std::getline(std::cin, line))
        {
            std::istringstream iss(line);
            std::string cmd;
            if (!(iss >> cmd))
                continue;

            if (cmd == "q" || cmd == "quit")
            {
                break;
            }
            else if (cmd == "text")
            {
                dump_text_now(audio);
            }
            else if (cmd == "dot")
            {
                dump_dot_now(audio, dot_path, false);
            }
            else if (cmd == "gain")
            {
                float g = 1.0f;
                if (!(iss >> g))
                {
                    std::cout << "Usage: gain <float>\n";
                    continue;
                }
                const RetCode rc = audio.set_gain(master_gain, g);
                std::cout << "set_gain(" << g << ") -> " << rc.what() << '\n';
            }
            else if (cmd == "high")
            {
                voice_high = !voice_high;
                const RetCode rc = audio.set_high_priority(main_mix[1], voice_high);
                std::cout << "voice high_priority=" << (voice_high ? "true" : "false") << " -> " << rc.what() << '\n';
            }
            else if (cmd == "mute")
            {
                extra_muted = !extra_muted;
                const RetCode rc = audio.set_muted(main_mix[2], extra_muted);
                std::cout << "extra muted=" << (extra_muted ? "true" : "false") << " -> " << rc.what() << '\n';
            }
            else if (cmd == "pause")
            {
                branch_paused = !branch_paused;
                check_rc(audio.set_paused(rec_paused, branch_paused), "set_paused");
                check_rc(audio.commit(), "commit");
                std::cout << "rec_paused=" << (branch_paused ? "paused" : "resumed") << " (committed)\n";
            }
            else
            {
                print_help();
            }
        }

        // 正常退出：排空队列并在析构时回填所有 WAV 文件头长度
        std::cout << "Stopping engine...\n";
        check_rc(audio.stop(), "stop");
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}