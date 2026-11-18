/**
 * @file scheduler.h
 * @brief Reorders the instructions of one straight-line segment to minimise pipeline stalls.
 *
 * A segment is a run of instructions with no label, directive, barrier or jump inside it. It may
 * end with one terminator (a branch or jump), which always stays last.
 *
 * Only instructions that are independent of each other are swapped: register dependencies
 * (RAW, WAR, WAW) and memory dependencies are all preserved, so the program computes exactly the
 * same result.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "reorder/reorder_types.h"

#include <cstddef>
#include <vector>

namespace Kites
{
namespace reorder
{

/// Segments with at most this many movable units are searched exhaustively.
constexpr size_t kExactSearchLimit = 12;

struct SegmentInput
{
    /// Movable units in original order (no barriers, no terminator). memory.baseVersion must
    /// already be filled in.
    std::vector<Unit> units;

    bool hasTerminator = false;
    Unit terminator;

    /// The last few micro-ops that run before this segment, oldest first.
    std::vector<MicroOp> history;
};

struct ScheduleResult
{
    /// New order, as indices into SegmentInput::units.
    std::vector<size_t> order;
    int stallsBefore = 0;
    int stallsAfter = 0;
    bool usedGreedy = false;
};

/// True if the two units must keep their relative order.
bool mustStayOrdered(const Unit &earlier, const Unit &later);

/// Picks the order of `input.units` with the fewest stalls for the given pipeline.
ScheduleResult scheduleSegment(const SegmentInput &input, PipelineMode mode,
                               size_t exactLimit = kExactSearchLimit);

} // namespace reorder
} // namespace Kites

#endif // SCHEDULER_H
