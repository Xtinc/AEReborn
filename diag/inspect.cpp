#include "inspect.h"

#include "backend/wasapi_backend.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <algorithm>
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
        out << static_cast<std::uint32_t>(spec.bandwidth) << '/' << spec.channels << "ch/" << format_name(spec.format);
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

struct MixerFlags
{
    std::size_t count = 0;
    std::uint64_t muted = 0, high = 0, suppressed = 0;

    std::uint64_t bit(std::size_t input) const noexcept
    {
        return count ? std::uint64_t{1} << input : 0;
    }
};

MixerFlags read_mixer_flags(const Node &node)
{
    MixerFlags flags;
    if (const auto *mixer = dynamic_cast<const Mixer *>(&node))
    {
        flags.count = mixer->inputs().size();
        for (std::size_t i = 0; i < flags.count; ++i)
        {
            flags.muted |= std::uint64_t{mixer->muted(i)} << i;
            flags.high |= std::uint64_t{mixer->high_priority(i)} << i;
            flags.suppressed |= std::uint64_t{mixer->suppressed(i)} << i;
        }
    }
    return flags;
}

void write_input_flags(std::ostream &out, const char *name, std::uint64_t flags, std::size_t count)
{
    out << ' ' << name << "=[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i)
            out << ", ";
        out << ((flags >> i) & 1);
    }
    out << ']';
}

void write_node_details(std::ostream &out, const Node &node, const MixerFlags &mixer)
{
    if (const auto *capture = dynamic_cast<const Capture *>(&node))
    {
        out << " | ";
        write_queue(out, *capture);
        out << " underrun=" << capture->underrun_frames();
    }
    else if (const auto *playback = dynamic_cast<const Playback *>(&node))
    {
        out << " | ";
        write_queue(out, *playback);
        out << " overflow=" << playback->overflow_frames();
    }
    else if (const auto *matrix = dynamic_cast<const Matrix *>(&node))
    {
        out << " | gain=" << std::fixed << std::setprecision(2) << matrix->gain();
    }
    else if (mixer.count)
    {
        out << " |";
        write_input_flags(out, "muted", mixer.muted, mixer.count);
        if (mixer.high || mixer.suppressed)
        {
            write_input_flags(out, "high", mixer.high, mixer.count);
            write_input_flags(out, "suppressed", mixer.suppressed, mixer.count);
        }
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
    out << " err=" << (error ? "OK" : error.what());
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

struct CardColors
{
    const char *fill;
    const char *border;
    const char *accent;
};

constexpr CardColors endpoint_colors{"#DFF4F0", "#2E8B7D", "#15803D"};
constexpr CardColors dsp_colors{"#E8EEF7", "#4A6FA5", "#1E4D83"};
constexpr CardColors backend_colors{"#F1F5F9", "#64748B", "#15803D"};
constexpr CardColors warning_colors{"#FEF3C7", "#D97706", "#92400E"};
constexpr CardColors error_colors{"#FEE2E2", "#DC2626", "#991B1B"};
constexpr CardColors inactive_colors{"#F3F4F6", "#9CA3AF", "#64748B"};

void write_dot_escaped(std::ostream &out, const std::string &text)
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

void write_html_escaped(std::ostream &out, const std::string &text)
{
    for (const auto ch : text)
    {
        switch (ch)
        {
        case '&': out << "&amp;"; break;
        case '<': out << "&lt;"; break;
        case '>': out << "&gt;"; break;
        case '"': out << "&quot;"; break;
        default: out << ch; break;
        }
    }
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
    case RetCode::EINPUTBUSY: return "EINPUTBUSY";
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

void write_short_rate(std::ostream &out, BandWidth rate)
{
    const auto hz = static_cast<std::uint32_t>(rate);
    out << hz / 1000;
    auto remainder = hz % 1000;
    if (remainder)
    {
        out << '.';
        for (auto place = 100u; remainder; place /= 10)
        {
            out << remainder / place;
            remainder %= place;
        }
    }
    out << 'k';
}

void write_short_spec(std::ostream &out, PortSpec spec)
{
    write_short_rate(out, spec.bandwidth);
    out << ' ' << spec.channels << "ch " << format_name(spec.format);
}

void begin_card(std::ostream &out, const CardColors &colors, const char *title, NodeId id = {}, bool ports = false)
{
    // Graphviz draws cell fills as rectangles. Keep the fill on the rounded table.
    out << " [label=<<TABLE BORDER=\"1\" CELLBORDER=\"0\" CELLSPACING=\"0\" CELLPADDING=\"5\""
           " STYLE=\"ROUNDED\" BGCOLOR=\"" << colors.fill << "\" COLOR=\"" << colors.border << '"';
    if (ports)
        out << " COLUMNS=\"*\"";
    out << "><TR><TD ALIGN=\"LEFT\"";
    if (ports)
        out << " COLSPAN=\"2\"";
    out << "><FONT POINT-SIZE=\"11\"><B>";
    write_html_escaped(out, title);
    out << "</B></FONT>";
    if (id)
        out << " <FONT POINT-SIZE=\"9\" COLOR=\"#475569\">#" << id.node() << "</FONT>";
}

void write_badge(std::ostream &out, const char *text, const CardColors &colors)
{
    out << " <FONT POINT-SIZE=\"9\" COLOR=\"" << colors.accent << "\"><B>" << text << "</B></FONT>";
}

void end_card(std::ostream &out, const std::string &details)
{
    out << "</TABLE>>, tooltip=\"";
    write_dot_escaped(out, details);
    out << "\"];\n";
}

template <class Endpoint>
void write_dot_endpoint(std::ostream &out, NodeId id, const Endpoint &endpoint, std::size_t lost, const char *loss_name)
{
    const auto online = endpoint.online();
    const auto steady = online && endpoint.steady();
    const auto &status_color = online ? (steady ? endpoint_colors : warning_colors) : inactive_colors;
    begin_card(out, lost ? warning_colors : status_color, endpoint.type_name(), id);
    write_badge(out, steady ? "ONLINE" : (online ? "PREFILL" : "OFFLINE"), status_color);
    out << "</TD></TR><HR/><TR><TD ALIGN=\"LEFT\">Queue ";

    std::size_t frames = 0;
    const auto capacity = endpoint.capacity();
    if (capacity && endpoint.estimate_queued_frames(frames))
    {
        const auto percent = static_cast<unsigned>(static_cast<double>(frames) * 100 / capacity + 0.5);
        out << frames << '/' << capacity << " (" << percent << "%)";
    }
    else
        out << "-/" << capacity;
    if (lost)
        out << "<BR ALIGN=\"LEFT\"/><FONT COLOR=\"" << warning_colors.accent << "\"><B>"
            << loss_name << ": " << lost << "f</B></FONT>";
    out << "</TD></TR>";
}

enum class InputStyle
{
    Normal,
    Muted,
    Suppressed
};

InputStyle input_style(const MixerFlags &mixer, std::uint64_t bit) noexcept
{
    if (mixer.muted & bit)
        return InputStyle::Muted;
    if (mixer.suppressed & ~mixer.high & bit)
        return InputStyle::Suppressed;
    return InputStyle::Normal;
}

struct DotNode
{
    View<const PortSpec> outputs;
    MixerFlags mixer;
    bool has_ports;
};

void write_port_cell(std::ostream &out, bool input, std::size_t index, std::size_t count, std::size_t rows,
                     InputStyle style, bool high = false)
{
    if (!count)
    {
        out << "<TD ROWSPAN=\"" << rows << "\"></TD>";
        return;
    }

    out << "<TD ALIGN=\"" << (input ? "LEFT" : "RIGHT") << "\" PORT=\"" << (input ? 'i' : 'o') << index << '"';
    if (rows > count && index == count - 1)
        out << " ROWSPAN=\"" << rows - index << '"';
    out << '>';
    if (style != InputStyle::Normal)
        out << "<FONT COLOR=\"" << (style == InputStyle::Muted ? inactive_colors.accent : warning_colors.accent) << "\">";
    out << (input ? "in" : "out") << index;
    if (style == InputStyle::Muted)
        out << (high ? " [HIGH MUTE]" : " [MUTE]");
    else if (style == InputStyle::Suppressed)
        out << " [SUPP]";
    else if (high)
        out << " [HIGH]";
    if (style != InputStyle::Normal)
        out << "</FONT>";
    out << "</TD>";
}

void write_dot_ports(std::ostream &out, const Node &node, const MixerFlags &mixer)
{
    const auto inputs = node.inputs().size();
    const auto outputs = node.outputs().size();
    const auto rows = std::max(inputs, outputs);
    for (std::size_t i = 0; i < rows; ++i)
    {
        out << "<TR>";
        if (i < inputs || (i == 0 && !inputs))
        {
            const auto bit = mixer.bit(i);
            write_port_cell(out, true, i, inputs, rows, input_style(mixer, bit), (mixer.high & bit) != 0);
        }
        if (i < outputs || (i == 0 && !outputs))
            write_port_cell(out, false, i, outputs, rows, InputStyle::Normal);
        out << "</TR>";
    }
}

void write_convert(std::ostream &out, PortSpec input, PortSpec output)
{
    const auto rate_changed = input.bandwidth != output.bandwidth;
    const auto format_changed = input.format != output.format;
    if (!rate_changed && !format_changed)
    {
        out << "passthrough";
        return;
    }

    const auto write_changed_fields = [&](PortSpec spec) {
        if (rate_changed)
            write_short_rate(out, spec.bandwidth);
        if (rate_changed && format_changed)
            out << ' ';
        if (format_changed)
            out << format_name(spec.format);
    };
    write_changed_fields(input);
    out << " -&gt; ";
    write_changed_fields(output);
}

void write_single_port_node(std::ostream &out, const Node &node)
{
    const auto inputs = node.inputs();
    const auto outputs = node.outputs();
    if (const auto *matrix = dynamic_cast<const Matrix *>(&node))
    {
        if (inputs[0].channels != outputs[0].channels)
            out << inputs[0].channels << "ch -&gt; " << outputs[0].channels << "ch | ";
        const auto gain = matrix->gain();
        if (gain == 0.0f)
            out << "<FONT COLOR=\"" << inactive_colors.accent << "\">Gain 0.00 [MUTE]</FONT>";
        else
            out << "Gain " << std::fixed << std::setprecision(2) << gain;
    }
    else if (dynamic_cast<const Convert *>(&node))
        write_convert(out, inputs[0], outputs[0]);
    else
    {
        write_short_spec(out, inputs.empty() ? outputs[0] : inputs[0]);
        if (!inputs.empty() && !outputs.empty() && inputs[0] != outputs[0])
        {
            out << " -&gt; ";
            write_short_spec(out, outputs[0]);
        }
    }
}

void write_dot_node(std::ostream &out, NodeId id, const Node &node, const DotNode &info)
{
    out << "  n" << id.node();
    if (const auto *capture = dynamic_cast<const Capture *>(&node))
        write_dot_endpoint(out, id, *capture, capture->underrun_frames(), "Underrun");
    else if (const auto *playback = dynamic_cast<const Playback *>(&node))
        write_dot_endpoint(out, id, *playback, playback->overflow_frames(), "Overflow");
    else
    {
        begin_card(out, dsp_colors, node.type_name(), id, info.has_ports);
        out << "</TD></TR><HR/>";
        if (info.has_ports)
            write_dot_ports(out, node, info.mixer);
        else
        {
            out << "<TR><TD ALIGN=\"LEFT\">";
            write_single_port_node(out, node);
            out << "</TD></TR>";
        }
    }

    std::ostringstream details;
    details.imbue(std::locale::classic());
    details << '[' << id.node() << "] " << node.type_name();
    write_ports(details, "in", node.inputs());
    write_ports(details, "out", node.outputs());
    write_node_details(details, node, info.mixer);
    end_card(out, details.str());
}

const char *write_dot_backend(std::ostream &out, const Backend &backend, RetCode error, bool detached)
{
    const auto *active = dynamic_cast<const ActiveBackend *>(&backend);
    const auto async = active && active->is_async();
    const auto fir = backend.uses_resampler();
    const auto state = active ? active->state() : (backend.running() ? ActiveBackend::State::Running
                                                                    : ActiveBackend::State::Stopped);
    const char *status = "STOP";
    auto colors = &inactive_colors;
    switch (state)
    {
    case ActiveBackend::State::Running:
        status = "RUN";
        colors = &backend_colors;
        break;
    case ActiveBackend::State::Draining:
        status = "DRAIN";
        colors = &warning_colors;
        break;
    case ActiveBackend::State::Stopping:
        status = "STOPPING";
        colors = &warning_colors;
        break;
    case ActiveBackend::State::Stopped:
        break;
    }
    if (!error)
        colors = &error_colors;
    begin_card(out, detached && error ? inactive_colors : *colors, backend.type_name());
    const auto badge = error ? std::string(status) : std::string("ERR: ") + error_code_name(error.code());
    write_badge(out, badge.c_str(), *colors);
    out << "</TD></TR><HR/><TR><TD ALIGN=\"LEFT\">";
    const auto device = backend.device_spec();
    if (device.channels)
    {
        write_short_spec(out, device);
        out << " (max " << backend.device_max_frames() << "f)";
    }
    else
        out << "unbound";
    out << "<BR ALIGN=\"LEFT\"/>" << (active ? (async ? "async" : "sync") : "passive");
    if (fir)
        out << " | FIR";
    if (async)
        out << " | drift " << std::fixed << std::setprecision(1) << std::showpos << active->drift_ppm()
            << std::noshowpos << " ppm";
    out << "</TD></TR>";

    std::ostringstream details;
    details.imbue(std::locale::classic());
    write_backend(details, backend, error);
    end_card(out, details.str());
    return async ? "ASRC" : (fir ? "FIR" : "PCM");
}

void write_dot_engine(std::ostream &out, const Engine &engine)
{
    const auto state = engine.state();
    const auto running = state == Engine::State::Running;
    const auto ready = state == Engine::State::Ready;
    const auto &colors = running ? endpoint_colors : (ready ? dsp_colors : inactive_colors);
    out << "<TABLE BORDER=\"0\" CELLBORDER=\"0\" CELLSPACING=\"4\" CELLPADDING=\"5\"><TR>"
           "<TD><FONT POINT-SIZE=\"15\"><B>AEReborn</B></FONT></TD><TD BGCOLOR=\"" << colors.fill << "\">";
    write_badge(out, running ? "RUNNING" : (ready ? "READY" : "UNPREPARED"), colors);
    out << "</TD><TD>" << static_cast<std::uint32_t>(engine.block_period()) << " ms | Plan #"
        << engine.plan_generation() << " | tick " << engine.tick_position() << " | " << engine.last_tick_us()
        << " us</TD>";
    if (engine.dirty())
    {
        out << "<TD BGCOLOR=\"" << warning_colors.fill << "\">";
        write_badge(out, "DIRTY", warning_colors);
        out << "</TD>";
    }
    const auto missed = engine.missed_periods();
    if (missed)
        out << "<TD BGCOLOR=\"" << warning_colors.fill << "\"><FONT POINT-SIZE=\"9\" COLOR=\""
            << warning_colors.accent << "\"><B>LATE " << missed << "</B></FONT></TD>";
    out << "</TR></TABLE>";
}

PortSpec connection_spec(const Engine &engine, const Connection &connection)
{
    return engine.plan_node(connection.from.base())->outputs()[connection.from.port()];
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
        out << (i ? " -> [" : " [") << nodes[i].node() << ']';

    BackendLinks links(backends);
    out << "\n\nNodes:\n";
    for (const auto id : nodes)
    {
        const auto &node = *engine.plan_node(id);
        out << "  [" << id.node() << "] " << std::left << std::setw(9) << node.type_name() << std::right;
        write_ports(out, "in", node.inputs());
        write_ports(out, "out", node.outputs());
        write_node_details(out, node, read_mixer_flags(node));
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
        out << "  #" << connection.id.value << ": [" << connection.from.node() << "]:" << connection.from.port()
            << " -> [" << connection.to.node() << "]:" << connection.to.port() << " (";
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
           "  graph [rankdir=LR, bgcolor=white, pad=0.32, nodesep=0.42, ranksep=0.64, labelloc=t, "
           "fontname=Arial, fontsize=11, fontcolor=\"#0F172A\", label=<";
    write_dot_engine(out, engine);
    out << ">, tooltip=\"";
    write_dot_escaped(out, details.str());
    out << "\"];\n"
           "  node [shape=plain, fontname=Arial, fontsize=10, fontcolor=\"#0F172A\"];\n"
           "  edge [fontname=Arial, fontsize=9, fontcolor=\"#64748B\", color=\"#475569\", arrowsize=0.7, penwidth=1.4];\n";

    BackendLinks links(backends);
    std::map<NodeId, DotNode> nodes;
    for (const auto id : engine.plan_nodes())
    {
        const auto &node = *engine.plan_node(id);
        const auto mixer = read_mixer_flags(node);
        const DotNode info{node.outputs(), mixer, node.inputs().size() > 1 || node.outputs().size() > 1 || mixer.high};
        nodes.emplace(id, info);
        write_dot_node(out, id, node, info);

        if (const auto *backend = links.take(node))
        {
            const auto error = backend->last_error();
            out << "  b" << id.node();
            const auto *mode = write_dot_backend(out, *backend, error, false);
            // A backend binds to a Capture source or a Playback sink.
            if (node.inputs().empty())
                out << "  b" << id.node() << ":e -> n" << id.node() << ":w";
            else
                out << "  n" << id.node() << ":e -> b" << id.node() << ":w";
            out << " [style=dashed, color=\"" << (error ? backend_colors.border : error_colors.border)
                << "\", label=\"" << mode << "\", tooltip=\"";
            write_dot_escaped(out, backend->type_name());
            out << " | graph ";
            write_spec(out, backend->logical_spec());
            out << " | device ";
            write_spec(out, backend->device_spec());
            out << "\"];\n";
        }
    }

    for (const auto &connection : engine.plan_connections())
    {
        const auto &from = nodes.find(connection.from.base())->second;
        const auto &to = nodes.find(connection.to.base())->second;
        const auto spec = from.outputs[connection.from.port()];
        const auto style =
            to.has_ports ? input_style(to.mixer, to.mixer.bit(connection.to.port())) : InputStyle::Normal;
        const auto *status = style == InputStyle::Muted ? "muted" : "suppressed";
        out << "  n" << connection.from.node();
        if (from.has_ports)
            out << ":o" << connection.from.port();
        out << ":e -> n" << connection.to.node();
        if (to.has_ports)
            out << ":i" << connection.to.port();
        out << ":w [label=\"";
        write_short_spec(out, spec);
        if (style != InputStyle::Normal)
        {
            const auto &colors = style == InputStyle::Muted ? inactive_colors : warning_colors;
            out << " (" << status << ")\", style=dashed, color=\"" << colors.border
                << "\", fontcolor=\"" << colors.accent << '"';
        }
        else
            out << '"';
        out << ", tooltip=\"#" << connection.id.value << ": [" << connection.from.node() << "]:" << connection.from.port()
            << " -> [" << connection.to.node() << "]:" << connection.to.port() << " (";
        write_spec(out, spec);
        if (style != InputStyle::Normal)
            out << ", " << status;
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
                out << "  subgraph cluster_detached {\n    label=\"Detached Backends\";\n    style=\"rounded,dashed\";\n"
                       "    fontname=Arial;\n    fontsize=10;\n    fontcolor=\"#64748B\";\n"
                       "    color=\"#9CA3AF\";\n    margin=12;\n";
                cluster_open = true;
            }
            out << "    detached" << i;
            write_dot_backend(out, *backend, backend->last_error(), true);
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
