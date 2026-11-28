#include "inspect.h"

#include "backend/wasapi_backend.h"
#include "backend/wav_backend.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>

namespace aereborn
{
namespace
{
const char *state_name(Engine::State state)
{
    switch (state)
    {
    case Engine::State::Unprepared: return "Unprepared";
    case Engine::State::Ready: return "Ready";
    case Engine::State::Running: return "Running";
    }
    return "?";
}

const char *state_name(ActiveBackend::State state)
{
    switch (state)
    {
    case ActiveBackend::State::Stopped: return "Stopped";
    case ActiveBackend::State::Running: return "Running";
    case ActiveBackend::State::Draining: return "Draining";
    case ActiveBackend::State::Stopping: return "Stopping";
    }
    return "?";
}

const char *format_name(SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::F32P: return "F32P";
    case SampleFormat::S16LE: return "S16LE";
    case SampleFormat::S24_3LE: return "S24_3LE";
    case SampleFormat::S24LE: return "S24LE";
    case SampleFormat::S32LE: return "S32LE";
    case SampleFormat::F32LE: return "F32LE";
    }
    return "?";
}

const char *error_code_name(RetCode::Code code)
{
    switch (code)
    {
    case RetCode::OK: return "OK";
    case RetCode::NOACTION: return "NOACTION";
    case RetCode::FAILED: return "FAILED";
    case RetCode::EPARAM: return "EPARAM";
    case RetCode::EPORT: return "EPORT";
    case RetCode::ETYPE: return "ETYPE";
    case RetCode::TOOLARGE: return "TOOLARGE";
    case RetCode::ESTATE: return "ESTATE";
    case RetCode::BUSY: return "BUSY";
    case RetCode::ENODE: return "ENODE";
    case RetCode::ECONN: return "ECONN";
    case RetCode::DUPLICATE: return "DUPLICATE";
    case RetCode::INPUTBUSY: return "INPUTBUSY";
    case RetCode::CYCLE: return "CYCLE";
    case RetCode::INVFMT: return "INVFMT";
    case RetCode::IOFAILED: return "IOFAILED";
    case RetCode::DEVLOST: return "DEVLOST";
    case RetCode::EOS: return "EOS";
    case RetCode::CANCELLED: return "CANCELLED";
    case RetCode::TIMEOUT: return "TIMEOUT";
    }
    return "Unknown";
}

void write_json_string(std::ostream &out, const std::string &text)
{
    const char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char ch : text)
    {
        if (ch == '"' || ch == '\\')
            out << '\\' << static_cast<char>(ch);
        else if (ch < 0x20)
            out << "\\u00" << hex[ch >> 4] << hex[ch & 0x0f];
        else
            out << static_cast<char>(ch);
    }
    out << '"';
}

void write_json_spec(std::ostream &out, PortSpec spec)
{
    out << "{\"rate\":" << static_cast<std::uint32_t>(spec.bandwidth)
        << ",\"channels\":" << spec.channels << ",\"format\":";
    write_json_string(out, format_name(spec.format));
    out << '}';
}

void write_json_ports(std::ostream &out, View<const PortSpec> ports)
{
    out << '[';
    for (std::size_t i = 0; i < ports.size(); ++i)
    {
        if (i)
            out << ',';
        write_json_spec(out, ports[i]);
    }
    out << ']';
}

template <class Endpoint>
void write_json_queue(std::ostream &out, const Endpoint &endpoint, const char *loss, std::size_t lost)
{
    const auto online = endpoint.online();
    const auto capacity = endpoint.capacity();
    std::size_t frames = 0;
    out << ",\"queue\":{\"bound\":" << endpoint.bound() << ",\"online\":" << online
        << ",\"steady\":" << (online && endpoint.steady()) << ",\"capacity\":" << capacity << ",\"frames\":";
    if (capacity && endpoint.estimate_queued_frames(frames))
        out << frames;
    else
        out << "null";
    out << ",\"" << loss << "\":" << lost << '}';
}

void write_json_node(std::ostream &out, const ConfiguredNode &entry, bool in_plan)
{
    const auto &node = *entry.node;
    out << "{\"id\":" << entry.id.node() << ",\"type\":";
    write_json_string(out, node.type_name());
    out << ",\"in_plan\":" << in_plan << ",\"paused\":" << entry.paused << ",\"inputs\":";
    write_json_ports(out, node.inputs());
    out << ",\"outputs\":";
    write_json_ports(out, node.outputs());
    if (const auto *capture = dynamic_cast<const Capture *>(&node))
        write_json_queue(out, *capture, "underrun", capture->underrun_frames());
    else if (const auto *playback = dynamic_cast<const Playback *>(&node))
        write_json_queue(out, *playback, "overflow", playback->overflow_frames());
    else if (const auto *matrix = dynamic_cast<const Matrix *>(&node))
        out << ",\"gain\":" << matrix->gain();
    else if (const auto *mixer = dynamic_cast<const Mixer *>(&node))
    {
        out << ",\"slots\":[";
        for (std::size_t i = 0; i < mixer->inputs().size(); ++i)
        {
            if (i)
                out << ',';
            out << "{\"high\":" << mixer->high_priority(i) << ",\"muted\":" << mixer->muted(i)
                << ",\"suppressed\":" << mixer->suppressed(i) << '}';
        }
        out << ']';
    }
    if (entry.device.state)
    {
        out << ",\"device_status\":{\"state\":";
        write_json_string(out, entry.device.state);
        out << ",\"error\":";
        write_json_string(out, error_code_name(entry.device.error.code()));
        out << '}';
    }
    out << '}';
}

void write_json_backend(std::ostream &out, const Backend &backend, NodeId endpoint)
{
    const auto *active = dynamic_cast<const ActiveBackend *>(&backend);
    using State = ActiveBackend::State;
    const auto state = active ? active->state() : (backend.running() ? State::Running : State::Stopped);
    const auto async = active && active->is_async();
    const auto error = backend.last_error();
    out << "{\"type\":";
    write_json_string(out, backend.type_name());
    out << ",\"node\":";
    if (endpoint)
        out << endpoint.node();
    else
        out << "null";
    out << ",\"state\":";
    write_json_string(out, state_name(state));
    out << ",\"error\":";
    write_json_string(out, error_code_name(error.code()));
    out << ",\"error_message\":";
    write_json_string(out, error.what());
    out << ",\"device\":";
    write_json_spec(out, backend.device_spec());
    out << ",\"max_frames\":" << backend.device_max_frames() << ",\"fir\":" << backend.uses_resampler()
        << ",\"async\":" << async << ",\"active\":" << (active != nullptr);
    if (async)
    {
        const auto drift = active->drift_ppm();
        if (std::isfinite(drift))
            out << ",\"drift_ppm\":" << drift;
    }
    const std::string *path = nullptr;
    if (const auto *capture = dynamic_cast<const WavCapture *>(&backend))
        path = &capture->path();
    else if (const auto *playback = dynamic_cast<const WavPlayback *>(&backend))
        path = &playback->path();
    if (path)
    {
        out << ",\"file\":";
        write_json_string(out, path->substr(path->find_last_of("/\\") + 1));
    }
#if WINDOWS_OS_ENVIRONMENT
    if (const auto *wasapi = dynamic_cast<const WasapiBackend *>(&backend))
    {
        const auto failure = wasapi->last_failure();
        if (failure.operation)
        {
            out << ",\"operation\":";
            write_json_string(out, failure.operation);
            out << ",\"hresult\":" << static_cast<std::uint32_t>(failure.hresult);
        }
    }
#endif
    out << '}';
}

struct ConfiguredTopology
{
    View<const ConfiguredNode> nodes;
    View<const Connection> connections;
};

std::string serialize_json(const Engine &engine, View<const Backend *const> backends, const ConfiguredTopology *configured)
{
    const auto plan = engine.plan_nodes();
    const auto committed_connections = engine.plan_connections();
    const auto connections = configured ? configured->connections : committed_connections;
    std::set<const Node *> plan_nodes;
    std::set<ConnId> plan_connections;
    if (configured)
        for (const auto &connection : committed_connections)
            plan_connections.insert(connection.id);

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::boolalpha << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\n  \"version\":1,\n  \"engine\":{\"state\":";
    write_json_string(out, state_name(engine.state()));
    out << ",\"period_ms\":" << static_cast<std::uint32_t>(engine.block_period())
        << ",\"plan\":" << engine.plan_generation() << ",\"tick\":" << engine.tick_position()
        << ",\"tick_us\":" << engine.last_tick_us() << ",\"missed_periods\":" << engine.missed_periods()
        << ",\"dirty\":" << engine.dirty() << "},\n  \"plan_order\":[";
    for (std::size_t i = 0; i < plan.size(); ++i)
    {
        if (configured)
            plan_nodes.insert(&engine.plan_node_at(i));
        if (i)
            out << ',';
        out << plan[i].node();
    }
    out << "],\n  \"nodes\":[";

    std::map<const Node *, NodeId> endpoint_ids;
    const auto node_count = configured ? configured->nodes.size() : plan.size();
    for (std::size_t i = 0; i < node_count; ++i)
    {
        const auto entry = configured ? configured->nodes[i] : ConfiguredNode{plan[i], &engine.plan_node_at(i), false};
        if (!backends.empty())
            endpoint_ids.emplace(entry.node, entry.id);
        out << (i ? ",\n    " : "\n    ");
        write_json_node(out, entry, !configured || plan_nodes.count(entry.node) != 0);
    }
    out << "\n  ],\n  \"connections\":[";
    for (std::size_t i = 0; i < connections.size(); ++i)
    {
        const auto &connection = connections[i];
        out << (i ? ",\n    " : "\n    ") << "{\"id\":" << connection.id.value
            << ",\"from\":[" << connection.from.node() << ',' << connection.from.port()
            << "],\"to\":[" << connection.to.node() << ',' << connection.to.port()
            << "],\"in_plan\":" << (!configured || plan_connections.count(connection.id) != 0) << '}';
    }
    out << "\n  ],\n  \"backends\":[";
    bool first = true;
    for (const auto *backend : backends)
    {
        if (!backend)
            continue;
        out << (first ? "\n    " : ",\n    ");
        first = false;
        const auto found = endpoint_ids.find(backend->endpoint_node());
        write_json_backend(out, *backend, found == endpoint_ids.end() ? NodeId{} : found->second);
    }
    out << "\n  ]\n}\n";
    return out.str();
}

} // namespace

std::string dump_json(const Engine &engine, View<const Backend *const> backends)
{
    return serialize_json(engine, backends, nullptr);
}

std::string dump_json(const Engine &engine, View<const ConfiguredNode> nodes, View<const Connection> connections,
                      View<const Backend *const> backends)
{
    const ConfiguredTopology configured{nodes, connections};
    return serialize_json(engine, backends, &configured);
}

} // namespace aereborn
