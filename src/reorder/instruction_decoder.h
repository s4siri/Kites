/**
 * @file instruction_decoder.h
 * @brief Splits assembly source lines and works out which registers each instruction reads and
 * writes.
 *
 * The decoder works on source text rather than on the assembler's output so that the reorderer
 * can hand back readable assembly with the user's formatting and comments intact.
 */

#ifndef INSTRUCTION_DECODER_H
#define INSTRUCTION_DECODER_H

#include "reorder/reorder_types.h"

#include <string>
#include <vector>

namespace Kites
{
namespace reorder
{

enum class LineKind
{
    Blank,       ///< nothing but whitespace, or only labels
    CommentOnly, ///< only a comment
    Directive,   ///< starts with '.'
    Instruction
};

/**
 * @brief A source line broken into labels and the statement that follows them.
 */
struct ParsedLine
{
    LineKind kind = LineKind::Blank;
    std::vector<std::string> labels; ///< labels defined at the start of the line
    std::string body;                ///< statement text, comment removed, trimmed
    size_t bodyOffset = 0;           ///< where the statement starts inside the original line
};

/// Splits a single source line (no trailing newline) into labels and statement.
ParsedLine parseLine(const std::string &line);

/// Returns the GPR number (0-31) for a register name such as "a0" or "x5", or -1.
int gprIndex(const std::string &name);

/// Returns the FPR number (0-31) for a register name such as "fa0" or "f3", or -1.
int fprIndex(const std::string &name);

/**
 * @brief Works out the micro-ops and dependencies of one instruction statement.
 *
 * Anything that is not understood is returned as a barrier so the reorderer leaves it, and its
 * neighbours, exactly where they are.
 *
 * @param body Statement text as returned in ParsedLine::body.
 * @return The decoded unit. memory.baseVersion and sourceIndex are filled in by the caller.
 */
Unit decodeInstruction(const std::string &body);

/// Number of machine instructions the assembler generates for `li rd, value`.
int liExpansionLength(uint64_t value);

} // namespace reorder
} // namespace Kites

#endif // INSTRUCTION_DECODER_H
