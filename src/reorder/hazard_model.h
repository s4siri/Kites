/**
 * @file hazard_model.h
 * @brief Static stall model for the Kites 5-stage pipelines.
 *
 * The rules mirror check_data_hazard() in processor/rv5s/rv5s_hdu.cpp, expressed as a minimum
 * distance, in decode-stage cycles, between a producer and a consumer of the same register:
 *
 *  - No forwarding: the value is only readable in the cycle the producer writes back, so the
 *    consumer must decode at least 3 cycles after the producer. Back-to-back dependent
 *    instructions therefore stall 2 cycles, and a gap of one instruction stalls 1 cycle.
 *  - Forwarding: only a load followed directly by a user of the loaded value stalls (1 cycle).
 *
 * Both x0 (never a hazard) and f0 (a normal register) are handled by RegSet. Control hazards are
 * not modelled: reordering never changes how many branches there are or where they sit.
 */

#ifndef HAZARD_MODEL_H
#define HAZARD_MODEL_H

#include "reorder/reorder_types.h"

#include <cstddef>
#include <vector>

namespace Kites
{
namespace reorder
{

/**
 * @brief Minimum number of decode cycles between `producer` and a dependent instruction.
 */
int dependencyLatency(PipelineMode mode, const MicroOp &producer);

/**
 * @brief Counts the stall cycles in a straight-line sequence of micro-ops.
 *
 * @param ops           The instructions in execution order.
 * @param firstCounted  Stalls are only counted for ops from this index on. The earlier ops serve
 *                      as history (for example the tail of the preceding basic block).
 * @param mode          Pipeline to model.
 */
int countStalls(const std::vector<const MicroOp *> &ops, size_t firstCounted, PipelineMode mode);

/// Convenience overload that counts every op in the sequence.
int countStalls(const std::vector<MicroOp> &ops, PipelineMode mode);

} // namespace reorder
} // namespace Kites

#endif // HAZARD_MODEL_H
