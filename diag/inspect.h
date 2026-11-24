#ifndef AEREBORN_DIAG_INSPECT_H
#define AEREBORN_DIAG_INSPECT_H

#include "core/engine.h"
#include "backend/backend.h"
#include <iosfwd>
#include <string>

namespace aereborn
{

/// @brief Write a text report of the committed render plan and supplied backends.
/// @pre Call on the serialized control side with live backends and endpoints.
/// @note Live figures are approximate and can represent different points in time.
void dump_text(std::ostream &out, const Engine &engine, View<const Backend *const> backends = {});

/// @brief Return the text report as a string. Follows the same contract as the stream overload.
std::string dump_text(const Engine &engine, View<const Backend *const> backends = {});

/// @brief Write a Graphviz DOT graph of the committed plan and supplied backends.
/// @copydetails dump_text(std::ostream &, const Engine &, View<const Backend *const>)
void dump_dot(std::ostream &out, const Engine &engine, View<const Backend *const> backends = {});

/// @brief Return the DOT graph as a string. Follows the same contract as the stream overload.
std::string dump_dot(const Engine &engine, View<const Backend *const> backends = {});

} // namespace aereborn

#endif
