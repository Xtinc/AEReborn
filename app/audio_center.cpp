#include "audio_interface.h"
#include "core/command_queue.h"
#include "graph_service.h"
#include <functional>
#include <new>

namespace aereborn
{
namespace
{

thread_local RetCode thread_last_error;

template <class Fn> auto dispatch(CommandQueue &commands, Fn &&fn) noexcept -> decltype(fn())
{
    thread_last_error = RetCode::OK;
    try
    {
        return commands.sync(fn);
    }
    catch (const Error &error)
    {
        thread_last_error = error.code();
    }
    catch (const std::bad_alloc &)
    {
        thread_last_error = RetCode::TOOLARGE;
    }
    catch (...)
    {
        thread_last_error = RetCode::FAILED;
    }
    return decltype(fn())();
}

template <class Fn> RetCode call_status(CommandQueue &commands, Fn &&fn) noexcept
{
    dispatch(commands, fn);
    return thread_last_error;
}

} // namespace

AudioCenter::AudioCenter(BlockPeriod period)
    : commands_(std::make_unique<CommandQueue>()), graph_(std::make_unique<GraphService>(*commands_, period))
{
}

AudioCenter::~AudioCenter()
{
    const auto shutdown = [this]() noexcept { graph_.reset(); };
    commands_->stop(std::cref(shutdown)); // stop() joins before the borrowed callable goes out of scope.
}

NodeId AudioCenter::create_iwav(const std::string &path, int repeat, std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] { return graph_->create_iwav(path, repeat, io_period); });
}

NodeId AudioCenter::create_owav(const std::string &path, PortSpec spec, std::chrono::seconds segment_duration,
                                std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] { return graph_->create_owav(path, spec, segment_duration, io_period); });
}

NodeId AudioCenter::create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs)
{
    return dispatch(*commands_, [&] { return graph_->create_mixer(bandwidth, channels, inputs); });
}

NodeId AudioCenter::create_matrix(BandWidth bandwidth, const ChannelMap &map, float gain)
{
    return dispatch(*commands_, [&] {
        throw_if_error(map.status());
        const auto &coefficients = map.coefficients();
        return graph_->create_matrix(bandwidth, map.input_channels(), map.output_channels(),
                                     {coefficients.data(), coefficients.size()}, gain);
    });
}

RetCode AudioCenter::connect(NodeId from, NodeId to)
{
    return call_status(*commands_, [&] { graph_->connect(from, to); });
}

RetCode AudioCenter::disconnect(NodeId from, NodeId to)
{
    return call_status(*commands_, [&] { graph_->disconnect(from, to); });
}

RetCode AudioCenter::disconnect(NodeId to)
{
    return call_status(*commands_, [&] { graph_->disconnect(to); });
}

RetCode AudioCenter::remove(NodeId node)
{
    return call_status(*commands_, [&] { graph_->remove(node); });
}

RetCode AudioCenter::commit()
{
    return call_status(*commands_, [&] { graph_->commit(); });
}

RetCode AudioCenter::start()
{
    return call_status(*commands_, [&] { graph_->start(); });
}

RetCode AudioCenter::stop() noexcept
{
    return call_status(*commands_, [&] { graph_->stop(); });
}

RetCode AudioCenter::set_paused(NodeId node, bool paused)
{
    return call_status(*commands_, [&] { graph_->set_paused(node, paused); });
}

RetCode AudioCenter::set_gain(NodeId matrix, float gain)
{
    return call_status(*commands_, [&] { graph_->set_gain(matrix, gain); });
}

RetCode AudioCenter::set_muted(NodeId mixer_port, bool muted)
{
    return call_status(*commands_, [&] { graph_->set_muted(mixer_port, muted); });
}

RetCode AudioCenter::set_high_priority(NodeId mixer_port, bool high)
{
    return call_status(*commands_, [&] { graph_->set_high_priority(mixer_port, high); });
}

std::string AudioCenter::dump_json() const
{
    return dispatch(*commands_, [&] { return graph_->dump_json(); });
}

RetCode AudioCenter::last_error() noexcept
{
    return thread_last_error;
}

} // namespace aereborn
