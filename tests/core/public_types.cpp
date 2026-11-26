#include "audio_types.h"

using namespace aereborn;

static_assert(SampleFormat{} == SampleFormat::F32P, "Preserve the zero-initialized sample format");
static_assert(RetCode{} == RetCode::OK && RetCode::NOACTION != RetCode{}, "Status values must be distinct");
static_assert(!NodeId::make(NodeId::max_node + 1), "Oversized node identities must not wrap");
constexpr auto port = NodeId::make(NodeId::max_node, NodeId::invalid_port - 1);
static_assert(port.node() == NodeId::max_node && port.port() == NodeId::invalid_port - 1,
              "The largest valid node and port must round-trip");
static_assert(port[0] == port.base(), "Port zero is the base node handle");
static_assert(port[65536].port() == NodeId::invalid_port, "Oversized port indices must not wrap");

int main()
{
    const PortSpec spec(BandWidth::CDQuality, 2, SampleFormat::S16LE);
    const auto period = BlockPeriod::INR_05MS;
    return spec.valid() && !PortSpec{}.valid() && spec.max_frames(period) == 221 &&
                   spec.frames(period, 0) == 221 && spec.frames(period, 1) == 220 &&
                   spec.frame_position(period, 2) == 441
               ? 0
               : 1;
}
