#ifndef AEREBORN_NODE_H
#define AEREBORN_NODE_H

#include "block.h"
#include <array>
#include <cstdint>
#include <stdexcept>

namespace aereborn
{
enum class ErrorCode
{
    InvalidArgument,
    UnknownNode,
    UnknownConnection,
    InvalidPort,
    InvalidNodeType,
    ChannelMismatch,
    DuplicateConnection,
    InputAlreadyConnected,
    Cycle,
    NotPrepared,
    BufferFull
};


} // namespace aereborn

#endif // AEREBORN_NODE_H