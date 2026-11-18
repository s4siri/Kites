/**
 * @file code_reorderer.h
 * @brief Entry point of the instruction reordering engine.
 *
 * Given RISC-V assembly source, produces a reordered copy that stalls less on the Kites 5-stage
 * pipeline, together with before/after stall counts.
 *
 * What stays untouched:
 *  - labels, directives and data (they split the code into independent blocks),
 *  - ecall/ebreak/fence/CSR instructions and anything not recognised (they act as barriers),
 *  - the relative order of dependent instructions (register and memory),
 *  - branches and jumps, which always remain the last instruction of their block.
 *
 * Comments and blank lines directly above an instruction move together with it. Pseudo
 * instructions such as `li` and `la` are moved as a whole, never split.
 */

#ifndef CODE_REORDERER_H
#define CODE_REORDERER_H

#include "reorder/reorder_types.h"

#include <string>

namespace Kites
{
namespace reorder
{

/**
 * @brief Reorders `source` for the given pipeline.
 *
 * Stall counts are static estimates for straight-line (fall-through) execution; they do not
 * depend on which paths a particular run takes. If reordering would not help, the original text
 * is returned unchanged.
 */
ReorderResult reorderProgram(const std::string &source, PipelineMode mode);

} // namespace reorder
} // namespace Kites

#endif // CODE_REORDERER_H
