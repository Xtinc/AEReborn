#ifndef AEREBORN_DIAG_INSPECT_H
#define AEREBORN_DIAG_INSPECT_H

#include "core/engine.h"
#include "backend/backend.h"
#include <string>

namespace aereborn
{

/// @brief Return versioned JSON of the committed plan and supplied backends.
/// @pre Call on the serialized control side with live backends and endpoints.
/// @note Metrics are sampled independently. WAV backends expose only the configured file name.
std::string dump_json(const Engine &engine, View<const Backend *const> backends = {});

struct ConfiguredNode
{
    NodeId id;
    const Node *node;
    bool paused;
};

/// @brief Return JSON of the caller's configured topology, marking members of the committed plan.
/// @pre Call on the serialized control side with live node and backend pointers.
std::string dump_json(const Engine &engine, View<const ConfiguredNode> nodes, View<const Connection> connections,
                      View<const Backend *const> backends);

} // namespace aereborn

#endif
