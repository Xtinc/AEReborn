#include "graph_service.h"
#include "backend/wav_backend.h"
#include "core/command_queue.h"
#include "diag/inspect.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <algorithm>
#include <cassert>
#include <utility>

namespace aereborn
{
namespace
{

std::vector<float> default_matrix(std::size_t inputs, std::size_t outputs)
{
    std::vector<float> coefficients;
    if (outputs > coefficients.max_size() / inputs)
        throw Error(RetCode::TOOLARGE);
    coefficients.resize(inputs * outputs);
    if (inputs == 1 || outputs == 1)
        std::fill(coefficients.begin(), coefficients.end(), 1.0f / static_cast<float>(inputs));
    else
        for (std::size_t ch = 0; ch < std::min(inputs, outputs); ++ch)
            coefficients[ch * inputs + ch] = 1.0f;
    return coefficients;
}

} // namespace

GraphService::GraphService(CommandQueue &commands, BlockPeriod period) : commands_(commands), engine_(period) {}

GraphService::~GraphService()
{
    assert(commands_.on_thread());
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
        if (it->second.backend)
            it->second.backend->unbind();
    engine_.stop();
}

std::map<NodeId, GraphService::NodeRecord>::iterator GraphService::add_node(std::unique_ptr<Node> node)
{
    const auto id = engine_.add_node(std::move(node));
    const auto cleanup = [&]() noexcept { engine_.remove_node(id); };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    const auto record = nodes_.emplace(id, NodeRecord{}).first;
    rollback.dismiss();
    return record;
}

template <class Endpoint>
NodeId GraphService::add_endpoint(std::unique_ptr<Endpoint> endpoint, std::unique_ptr<ActiveBackend> backend)
{
    auto *node = endpoint.get();
    node->prepare(engine_.block_period());
    const auto record = add_node(std::move(endpoint));
    const auto cleanup = [&]() noexcept {
        engine_.remove_node(record->first);
        nodes_.erase(record);
    };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    backend->bind(*node);
    record->second.backend = std::move(backend);
    rollback.dismiss();
    return record->first;
}

NodeId GraphService::create_iwav(const std::string &path, int repeat, std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    auto backend = repeat > 0 ? std::make_unique<WavCapture>(path, WavCapture::Repeat{static_cast<std::size_t>(repeat)})
                              : std::make_unique<WavCapture>(path, repeat < 0);
    const auto spec = backend->probe().value_or_throw();
    auto endpoint = std::make_unique<Capture>(spec.bandwidth, spec.channels, io_period);
    return add_endpoint(std::move(endpoint), std::move(backend));
}

NodeId GraphService::create_owav(const std::string &path, PortSpec spec, std::chrono::seconds segment_duration,
                                 std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    if (!spec.valid())
        throw Error(RetCode::EPARAM);
    throw_if_error(WavFile::max_write_frames(spec).ret_code());
    auto endpoint = std::make_unique<Playback>(spec.bandwidth, spec.channels, io_period);
    auto backend = std::make_unique<WavPlayback>(path, spec.format, segment_duration, WavFile::WriteMode::Exclusive);
    return add_endpoint(std::move(endpoint), std::move(backend));
}

NodeId GraphService::create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs)
{
    assert(commands_.on_thread());
    return add_node(std::make_unique<Mixer>(bandwidth, channels, inputs))->first;
}

NodeId GraphService::create_matrix(BandWidth bandwidth, std::size_t channels, float gain)
{
    assert(commands_.on_thread());
    return add_node(std::make_unique<Matrix>(bandwidth, channels, gain))->first;
}

NodeId GraphService::create_matrix(BandWidth bandwidth, std::size_t input_channels, std::size_t output_channels,
                                   View<const float> coefficients, float gain)
{
    assert(commands_.on_thread());
    return add_node(std::make_unique<Matrix>(bandwidth, input_channels, output_channels, coefficients, gain))->first;
}

std::map<NodeId, GraphService::NodeRecord>::iterator GraphService::find_node(NodeId node)
{
    assert(commands_.on_thread());
    if (node.port() != 0)
        throw Error(RetCode::EPORT);
    const auto record = nodes_.find(node);
    if (record == nodes_.end())
        throw Error(RetCode::ENODE);
    return record;
}

PortSpec GraphService::port_spec(NodeId port, bool output)
{
    find_node(port.base());
    const auto *node = engine_.node(port.base());
    const auto ports = output ? node->outputs() : node->inputs();
    if (port.port() >= ports.size())
        throw Error(RetCode::EPORT);
    return ports[port.port()];
}

void GraphService::connect(NodeId from, NodeId to)
{
    auto source = port_spec(from, true);
    const auto destination = port_spec(to, false);
    assert(source.format == SampleFormat::F32P && destination.format == SampleFormat::F32P);
    const auto found = links_.lower_bound(to);
    if (found != links_.end() && found->first == to)
        throw Error(found->second.from == from ? RetCode::DUPLICATE : RetCode::INPUTBUSY);

    const auto entry = links_.emplace_hint(found, to, LogicalLink{from});
    auto &link = entry->second;
    const auto cleanup = [&]() noexcept {
        remove_bridges(link);
        links_.erase(entry);
    };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    std::size_t next = 0;
    auto previous = from;
    const auto append = [&](NodeId bridge) {
        link.bridges[next++] = bridge;
        engine_.connect(previous, bridge);
        previous = bridge;
    };
    const auto map_channels = [&] {
        const auto coefficients = default_matrix(source.channels, destination.channels);
        append(engine_.emplace_node<Matrix>(source.bandwidth, source.channels, destination.channels,
                                            View<const float>{coefficients.data(), coefficients.size()}));
        source.channels = destination.channels;
    };

    if (source.channels > destination.channels)
        map_channels();
    if (source.bandwidth != destination.bandwidth)
    {
        const PortSpec converted{destination.bandwidth, source.channels, source.format};
        append(engine_.emplace_node<Convert>(source, converted));
        source = converted;
    }
    if (source.channels < destination.channels)
        map_channels();
    const auto connection = engine_.connect(previous, to);
    if (next == 0)
        link.direct = connection;
    rollback.dismiss();
}

void GraphService::remove_bridges(const LogicalLink &link) noexcept
{
    // Removing a bridge also removes every connection incident to it.
    for (auto it = link.bridges.rbegin(); it != link.bridges.rend(); ++it)
        if (*it)
            engine_.remove_node(*it);
}

void GraphService::disconnect(NodeId from, NodeId to)
{
    disconnect_input(to, &from);
}

void GraphService::disconnect(NodeId to)
{
    disconnect_input(to, nullptr);
}

void GraphService::disconnect_input(NodeId to, const NodeId *from)
{
    assert(commands_.on_thread());
    const auto link = links_.find(to);
    if (link == links_.end() || (from && link->second.from != *from))
    {
        if (from)
            port_spec(*from, true);
        port_spec(to, false);
        throw Error(RetCode::ECONN);
    }
    if (link->second.direct)
        engine_.disconnect(link->second.direct);
    else
        remove_bridges(link->second);
    links_.erase(link);
}

void GraphService::remove(NodeId node)
{
    const auto record = find_node(node);
    if (record->second.backend)
        record->second.backend->unbind();
    for (auto it = links_.begin(); it != links_.end();)
    {
        if (it->first.base() == node || it->second.from.base() == node)
        {
            remove_bridges(it->second);
            it = links_.erase(it);
        }
        else
            ++it;
    }
    engine_.remove_node(node);
    nodes_.erase(record);
}

void GraphService::sync_backends()
{
    const auto plan = engine_.plan_nodes();
    for (auto &entry : nodes_)
    {
        auto &record = entry.second;
        if (!record.backend)
            continue;
        const auto in_plan = std::find(plan.begin(), plan.end(), entry.first) != plan.end();
        if (in_plan && !record.started_in_plan)
        {
            record.backend->start();
            record.started_in_plan = true;
        }
        else if (!in_plan && record.started_in_plan)
        {
            record.backend->stop();
            record.started_in_plan = false;
        }
    }
}

void GraphService::commit()
{
    assert(commands_.on_thread());
    if (!engine_.dirty())
        return;
    engine_.commit();
    if (engine_.state() == Engine::State::Running)
    {
        const auto cleanup = [this]() noexcept { stop(); };
        ScopeExit<decltype(cleanup)> rollback(cleanup);
        sync_backends();
        rollback.dismiss();
    }
}

void GraphService::start()
{
    assert(commands_.on_thread());
    if (engine_.state() == Engine::State::Running)
        throw Error(RetCode::BUSY);
    engine_.commit();
    const auto cleanup = [this]() noexcept { stop(); };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    sync_backends();
    engine_.start();
    rollback.dismiss();
}

void GraphService::stop() noexcept
{
    assert(commands_.on_thread());
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
    {
        auto &record = it->second;
        if (record.started_in_plan)
        {
            record.backend->stop();
            record.started_in_plan = false;
        }
    }
    engine_.stop();
}

void GraphService::set_paused(NodeId node, bool paused)
{
    find_node(node);
    engine_.set_paused(node, paused);
}

void GraphService::set_gain(NodeId matrix, float gain)
{
    find_node(matrix);
    engine_.node<Matrix>(matrix)->set_gain(gain);
}

void GraphService::set_muted(NodeId mixer_port, bool muted)
{
    find_node(mixer_port.base());
    engine_.node<Mixer>(mixer_port.base())->set_muted(mixer_port.port(), muted);
}

void GraphService::set_high_priority(NodeId mixer_port, bool high)
{
    find_node(mixer_port.base());
    engine_.node<Mixer>(mixer_port.base())->set_high_priority(mixer_port.port(), high);
}

std::vector<const Backend *> GraphService::backends() const
{
    assert(commands_.on_thread());
    std::vector<const Backend *> result;
    for (const auto &entry : nodes_)
        if (entry.second.backend)
            result.push_back(entry.second.backend.get());
    return result;
}

std::string GraphService::dump_text() const
{
    const auto drivers = backends();
    return aereborn::dump_text(engine_, {drivers.data(), drivers.size()});
}

std::string GraphService::dump_dot() const
{
    const auto drivers = backends();
    return aereborn::dump_dot(engine_, {drivers.data(), drivers.size()});
}

} // namespace aereborn
