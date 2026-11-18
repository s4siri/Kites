/**
 * @file reorder_types.h
 * @brief Shared data types for the instruction reordering engine.
 *
 * The engine is intentionally free of any Qt dependency so that it can be unit tested on its own
 * and reused from the CLI later.
 */

#ifndef REORDER_TYPES_H
#define REORDER_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Kites
{
namespace reorder
{

/**
 * @brief Which pipeline the reordered code is meant for.
 *
 * The two modes use different hazard rules (see hazard_model.h) and therefore usually produce
 * different schedules for the same source.
 */
enum class PipelineMode
{
    NoForwarding,
    Forwarding
};

/**
 * @brief A set of architectural registers, one bit per register.
 *
 * x0 is never stored in a set (it can neither create nor suffer a hazard), but f0 is a normal
 * register and is stored.
 */
struct RegSet
{
    uint32_t gpr = 0;
    uint32_t fpr = 0;

    void addGpr(int index)
    {
        if (index > 0 && index < 32)
        {
            gpr |= (1u << index);
        }
    }

    void addFpr(int index)
    {
        if (index >= 0 && index < 32)
        {
            fpr |= (1u << index);
        }
    }

    bool intersects(const RegSet &other) const
    {
        return (gpr & other.gpr) != 0 || (fpr & other.fpr) != 0;
    }

    bool empty() const
    {
        return gpr == 0 && fpr == 0;
    }

    void merge(const RegSet &other)
    {
        gpr |= other.gpr;
        fpr |= other.fpr;
    }
};

/**
 * @brief One real machine instruction (what the pipeline sees).
 *
 * A pseudo-instruction such as `li` or `la` can expand to several of these.
 */
struct MicroOp
{
    RegSet reads;
    RegSet writes;
    bool isLoad = false;
};

/**
 * @brief Description of the memory location touched by a load or store.
 *
 * Two accesses that use the same base register, with the base unchanged in between, and whose
 * byte ranges do not overlap are known not to alias.
 */
struct MemoryAccess
{
    bool valid = false; ///< false => address unknown, assume it may alias anything
    int baseRegister = 0;
    int baseVersion = 0; ///< number of writes to baseRegister seen so far in the block
    int64_t offset = 0;
    int size = 0;
};

/**
 * @brief A schedulable unit: one source line, i.e. one or more micro-ops that must stay together.
 */
struct Unit
{
    std::vector<MicroOp> ops;

    RegSet reads;  ///< registers read from outside the unit
    RegSet writes; ///< registers written by the unit

    bool isLoad = false;
    bool isStore = false;
    MemoryAccess memory;

    bool isBarrier = false;    ///< nothing may move across it (ecall, csr, fence, unknown ...)
    bool isTerminator = false; ///< ends a basic block (branch / jump); always stays last

    size_t sourceIndex = 0; ///< position in the original block
};

/**
 * @brief Counts reported for one run of the reorderer.
 */
struct ReorderStats
{
    int instructionCount = 0;       ///< machine instructions (pseudo-instructions expanded)
    int stallsBefore = 0;
    int stallsAfter = 0;
    int blockCount = 0;
    int blocksChanged = 0;
    int barrierLineCount = 0;       ///< lines kept in place because they were not understood
    bool usedGreedyFallback = false; ///< true if at least one block was too big for exact search

    /// Cycles for the original code assuming a classic 5-stage pipeline: fill + one per
    /// instruction + stalls.
    int cyclesBefore() const
    {
        return instructionCount == 0 ? 0 : instructionCount + 4 + stallsBefore;
    }

    int cyclesAfter() const
    {
        return instructionCount == 0 ? 0 : instructionCount + 4 + stallsAfter;
    }
};

/**
 * @brief The result of reordering a program.
 */
struct ReorderResult
{
    std::string originalText;
    std::string reorderedText;
    ReorderStats stats;
};

} // namespace reorder
} // namespace Kites

#endif // REORDER_TYPES_H
