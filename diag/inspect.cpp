#include "inspect.h"

#include "backend/wasapi_backend.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <iomanip>
#include <locale>
#include <map>
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

void write_spec(std::ostream &out, PortSpec spec)
{
    if (spec.channels)
        out << static_cast<std::uint32_t>(spec.sample_rate) << '/' << spec.channels << "ch/" << format_name(spec.format);
    else
        out << '-';
}

void write_ports(std::ostream &out, const char *name, View<const PortSpec> ports)
{
    for (std::size_t first = 0; first < ports.size();)
    {
        auto last = first;
        while (last + 1 < ports.size() && ports[last + 1] == ports[first])
            ++last;
        out << ' ' << name << first;
        if (last != first)
            out << ".." << last;
        out << '=';
        write_spec(out, ports[first]);
        first = last + 1;
    }
}

template <class Endpoint> void write_queue(std::ostream &out, const Endpoint &endpoint)
{
    const auto online = endpoint.online();
    out << "bound=" << endpoint.bound() << " online=" << online << " steady=" << (online && endpoint.steady())
        << " fill=";
    std::size_t frames = 0;
    if (endpoint.estimate_queued_frames(frames))
        out << frames;
    else
        out << "\xE2\x80\x94"; // Em dash.
    out << '/' << endpoint.capacity();
}

void write_node_details(std::ostream &out, const Node &node, const char *separator)
{
    if (const auto *capture = dynamic_cast<const Capture *>(&node))
    {
        out << separator;
        write_queue(out, *capture);
        out << " underrun=" << capture->underrun_frames();
    }
    else if (const auto *playback = dynamic_cast<const Playback *>(&node))
    {
        out << separator;
        write_queue(out, *playback);
        out << " overflow=" << playback->overflow_frames();
    }
    else if (const auto *matrix = dynamic_cast<const Matrix *>(&node))
    {
        out << separator << "gain=" << std::fixed << std::setprecision(2) << matrix->gain();
    }
    else if (const auto *mixer = dynamic_cast<const Mixer *>(&node))
    {
        out << separator << "muted=[";
        for (std::size_t i = 0; i < mixer->inputs().size(); ++i)
        {
            if (i)
                out << ", ";
            out << mixer->muted(i);
        }
        out << ']';
    }
}

void write_backend(std::ostream &out, const Backend &backend, RetCode error)
{
    out << backend.type_name();
    const auto *active = dynamic_cast<const ActiveBackend *>(&backend);
    const auto async = active && active->is_async();
    out << " (" << (async ? "async" : "sync") << ") state=";
    out << (active ? state_name(active->state()) : (backend.running() ? "Running" : "Stopped"));
    out << " dev=";
    write_spec(out, backend.device_spec());
    out << " max=" << backend.device_max_frames() << " fir=" << backend.uses_resampler();
    if (async)
        out << " drift=" << std::fixed << std::setprecision(1) << std::showpos << active->drift_ppm()
            << std::noshowpos << "ppm";
    out << " err=" << (error ? "Ok" : error.message());
#if WINDOWS_OS_ENVIRONMENT
    if (const auto *wasapi = dynamic_cast<const WasapiBackend *>(&backend))
    {
        const auto failure = wasapi->last_failure();
        if (failure.operation)
            out << " (" << failure.operation << " HRESULT=0x" << std::hex
                << static_cast<std::uint32_t>(failure.hresult) << std::dec << ')';
    }
#endif
}

class BackendLinks
{
  public:
    explicit BackendLinks(View<const Backend *const> backends)
    {
        for (const auto *backend : backends)
            if (backend)
                if (const auto *endpoint = backend->endpoint_node())
                    unvisited_.emplace(endpoint, backend);
    }

    const Backend *take(const Node &node)
    {
        const auto found = unvisited_.find(&node);
        if (found == unvisited_.end())
            return nullptr;
        const auto *backend = found->second;
        unvisited_.erase(found);
        return backend;
    }

    bool detached(const Backend &backend) const
    {
        const auto *endpoint = backend.endpoint_node();
        return !endpoint || unvisited_.count(endpoint);
    }

  private:
    std::map<const Node *, const Backend *> unvisited_;
};

void write_engine(std::ostream &out, const Engine &engine)
{
    out << "Engine: " << state_name(engine.state()) << " | period=" << static_cast<std::uint32_t>(engine.block_period())
        << "ms | tick=" << engine.tick_position() << " | plan_gen=" << engine.plan_generation()
        << " | dirty=" << (engine.dirty() ? "true" : "false") << " | late=" << engine.missed_periods()
        << " | tick_us=" << engine.last_tick_us();
}

constexpr auto online_symbol = u8"\u25CF";
constexpr auto prefill_symbol = u8"\u25D0";
constexpr auto offline_symbol = u8"\u25CB";

void write_dot_label(std::ostream &out, const std::string &text)
{
    for (const auto ch : text)
    {
        if (ch == '\r')
            continue;
        if (ch == '\n')
        {
            out << "\\n";
            continue;
        }
        if (ch == '"' || ch == '\\')
            out << '\\';
        out << ch;
    }
}

template <class Endpoint>
void write_dot_endpoint(std::ostream &out, NodeId id, const Endpoint &endpoint, std::size_t lost)
{
    const auto online = endpoint.online();
    out << "label=\"" << (online ? (endpoint.steady() ? online_symbol : prefill_symbol) : offline_symbol)
        << ' ' << endpoint.type_name() << "  #" << id.value << "\\n";

    std::size_t frames = 0;
    const auto capacity = endpoint.capacity();
    if (capacity && endpoint.estimate_queued_frames(frames))
    {
        constexpr auto segments = 5;
        const auto filled = static_cast<int>(static_cast<double>(frames) * segments / capacity + 0.5);
        for (int segment = 0; segment < segments; ++segment)
            out << (segment < filled ? u8"\u25B0" : u8"\u25B1");
        out << "  " << frames << '/' << capacity;
    }
    else
        out << u8"\u2014";
    if (lost)
        out << "  ! " << lost;
    out << "\", fillcolor=" << (lost ? "\"#FFF2DF\"" : "\"#E7F5F2\"")
        << ", color=" << (lost ? "\"#C88643\"" : "\"#7BAFA8\"");
}

void write_dot_node(std::ostream &out, NodeId id, const Node &node)
{
    out << "  n" << id.value << " [";
    if (const auto *capture = dynamic_cast<const Capture *>(&node))
        write_dot_endpoint(out, id, *capture, capture->underrun_frames());
    else if (const auto *playback = dynamic_cast<const Playback *>(&node))
        write_dot_endpoint(out, id, *playback, playback->overflow_frames());
    else
    {
        out << "label=\"";
        const auto *mixer = dynamic_cast<const Mixer *>(&node);
        const auto *matrix = dynamic_cast<const Matrix *>(&node);
        const auto *convert = dynamic_cast<const Convert *>(&node);
        if (mixer)
            out << u8"\u03A3 ";
        else if (convert)
            out << u8"\u2194 ";
        write_dot_label(out, node.type_name());
        out << "  #" << id.value;
        if (mixer)
            out << "\\n" << mixer->inputs().size() << " inputs";
        else if (matrix)
            out << u8"\\n\u00D7 " << std::fixed << std::setprecision(2) << matrix->gain() << std::defaultfloat
                << std::setprecision(6);
        else if (convert)
        {
            const auto input = convert->inputs()[0];
            const auto output = convert->outputs()[0];
            if (input.sample_rate != output.sample_rate)
                out << "\\n" << static_cast<std::uint32_t>(input.sample_rate) / 1000.0 << u8" \u2192 "
                    << static_cast<std::uint32_t>(output.sample_rate) / 1000.0 << " kHz";
            else if (input.format != output.format)
                out << "\\n" << format_name(input.format) << u8" \u2192 " << format_name(output.format);
        }
        out << '"';
    }

    std::ostringstream details;
    details.imbue(std::locale::classic());
    details << '[' << id.value << "] " << node.type_name();
    write_ports(details, "in", node.inputs());
    write_ports(details, "out", node.outputs());
    write_node_details(details, node, " | ");
    out << ", tooltip=\"";
    write_dot_label(out, details.str());
    out << "\"];\n";
}

void write_dot_backend(std::ostream &out, const Backend &backend, bool detached)
{
    const auto error = backend.last_error();
    out << " [label=\"" << (error ? (backend.running() ? online_symbol : offline_symbol) : "!") << ' ';
    write_dot_label(out, backend.type_name());
    const auto device = backend.device_spec();
    if (device.channels)
        out << "\\n" << static_cast<std::uint32_t>(device.sample_rate) / 1000.0 << " kHz / " << device.channels << " ch";
    out << "\", fontsize=10, fillcolor=" << (error ? "\"#F7F9FC\"" : "\"#FCEBE8\"");
    if (!error)
        out << ", color=\"#C76662\", fontcolor=\"#9F3836\"";
    if (detached)
        out << ", style=\"rounded,filled,dashed\"";

    std::ostringstream details;
    details.imbue(std::locale::classic());
    write_backend(details, backend, error);
    out << ", tooltip=\"";
    write_dot_label(out, details.str());
    out << "\"];\n";
}

PortSpec connection_spec(const Engine &engine, const Connection &connection)
{
    return engine.plan_node(connection.from.node)->outputs()[connection.from.index];
}
} // namespace

std::string dump_text(const Engine &engine, View<const Backend *const> backends)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    write_engine(out, engine);
    const auto nodes = engine.plan_nodes();
    out << "\nPlan Order:";
    for (std::size_t i = 0; i < nodes.size(); ++i)
        out << (i ? " -> [" : " [") << nodes[i].value << ']';

    BackendLinks links(backends);
    out << "\n\nNodes:\n";
    for (const auto id : nodes)
    {
        const auto &node = *engine.plan_node(id);
        out << "  [" << id.value << "] " << std::left << std::setw(9) << node.type_name() << std::right;
        write_ports(out, "in", node.inputs());
        write_ports(out, "out", node.outputs());
        write_node_details(out, node, " | ");
        out << '\n';
        if (const auto *backend = links.take(node))
        {
            out << "      \xE2\x94\x94\xE2\x94\x80 ";
            write_backend(out, *backend, backend->last_error());
            out << '\n';
        }
    }

    out << "\nConnections:\n";
    for (const auto &connection : engine.plan_connections())
    {
        out << "  #" << connection.id.value << ": [" << connection.from.node.value << "]:" << connection.from.index
            << " -> [" << connection.to.node.value << "]:" << connection.to.index << " (";
        write_spec(out, connection_spec(engine, connection));
        out << ")\n";
    }

    bool heading_written = false;
    for (const auto *backend : backends)
        if (backend && links.detached(*backend))
        {
            if (!heading_written)
            {
                out << "\nDetached Backends:\n";
                heading_written = true;
            }
            out << "  ";
            write_backend(out, *backend, backend->last_error());
            out << '\n';
        }
    return out.str();
}

void dump_text(std::ostream &out, const Engine &engine, View<const Backend *const> backends)
{
    const auto report = dump_text(engine, backends);
    out.write(report.data(), static_cast<std::streamsize>(report.size()));
}

std::string dump_dot(const Engine &engine, View<const Backend *const> backends)
{
    std::ostringstream out, details;
    out.imbue(std::locale::classic());
    details.imbue(std::locale::classic());
    write_engine(details, engine);
    out << "digraph AEReborn {\n"
           "  graph [rankdir=LR, bgcolor=white, pad=0.28, nodesep=0.34, ranksep=0.48, labelloc=t, "
           "fontname=Arial, fontsize=17, fontcolor=\"#172D48\", label=<<B>AEReborn</B><BR/>"
           "<FONT FACE=\"Arial\" POINT-SIZE=\"11\" COLOR=\"#52677A\">"
        << state_name(engine.state()) << "  /  " << static_cast<std::uint32_t>(engine.block_period())
        << " ms  /  Plan " << engine.plan_generation()
        << "</FONT><BR/><FONT FACE=\"Arial\" POINT-SIZE=\"10\" COLOR=\"#52677A\">"
        << online_symbol << " online   " << prefill_symbol << " prefill   " << offline_symbol
        << " offline   ! alert</FONT>>, tooltip=\"";
    write_dot_label(out, details.str());
    out << "\"];\n"
           "  node [shape=box, style=\"rounded,filled\", fontname=Arial, fontsize=12, "
           "fontcolor=\"#172D48\", fillcolor=\"#EEF3FA\", color=\"#ADC0D7\", margin=\"0.13,0.08\"];\n"
           "  edge [color=\"#607C9A\", arrowsize=0.65, penwidth=1.4];\n";

    BackendLinks links(backends);
    for (const auto id : engine.plan_nodes())
    {
        const auto &node = *engine.plan_node(id);
        write_dot_node(out, id, node);

        if (const auto *backend = links.take(node))
        {
            out << "  b" << id.value;
            write_dot_backend(out, *backend, false);
            // A backend binds to a Capture source or a Playback sink.
            if (node.inputs().empty())
                out << "  b" << id.value << " -> n" << id.value;
            else
                out << "  n" << id.value << " -> b" << id.value;
            out << " [style=dashed, color=\"#8297AD\"];\n";
        }
    }

    for (const auto &connection : engine.plan_connections())
    {
        out << "  n" << connection.from.node.value << " -> n" << connection.to.node.value << " [tooltip=\"#"
            << connection.id.value << ": [" << connection.from.node.value << "]:" << connection.from.index
            << " -> [" << connection.to.node.value << "]:" << connection.to.index << " (";
        write_spec(out, connection_spec(engine, connection));
        out << ")\"];\n";
    }

    bool cluster_open = false;
    for (std::size_t i = 0; i < backends.size(); ++i)
    {
        const auto *backend = backends[i];
        if (backend && links.detached(*backend))
        {
            if (!cluster_open)
            {
                out << "  subgraph cluster_detached {\n    label=\"Detached\";\n    style=\"rounded,dashed\";\n"
                       "    fontname=Arial;\n    fontsize=10;\n    fontcolor=\"#52677A\";\n"
                       "    color=\"#ADC0D7\";\n    margin=12;\n";
                cluster_open = true;
            }
            out << "    detached" << i;
            write_dot_backend(out, *backend, true);
        }
    }
    if (cluster_open)
        out << "  }\n";
    out << "}\n";
    return out.str();
}

void dump_dot(std::ostream &out, const Engine &engine, View<const Backend *const> backends)
{
    const auto graph = dump_dot(engine, backends);
    out.write(graph.data(), static_cast<std::streamsize>(graph.size()));
}

} // namespace aereborn
