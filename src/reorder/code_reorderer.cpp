#include "reorder/code_reorderer.h"

#include "reorder/hazard_model.h"
#include "reorder/instruction_decoder.h"
#include "reorder/scheduler.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace Kites
{
namespace reorder
{

namespace
{

/// How many earlier micro-ops are passed to the scheduler as history.
constexpr size_t kHistoryLength = 3;

struct Entry
{
    bool isInstruction = false;
    std::vector<std::string> leading; ///< comment/blank lines that travel with the instruction
    std::string text;                 ///< the line to print
    Unit unit;                        ///< valid when isInstruction
};

std::string trimmed(const std::string &text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
    {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// Looks at a directive and returns +1 if it switches to a data-like section, -1 if it switches
/// to code, 0 if it does not change the section.
int sectionChange(const std::string &directive)
{
    std::string lower = lowered(directive);
    size_t split = lower.find_first_of(" \t");
    std::string name = lower.substr(0, split);
    std::string argument =
        (split == std::string::npos) ? std::string() : trimmed(lower.substr(split));

    auto isDataName = [](const std::string &section)
    {
        return section.rfind(".data", 0) == 0 || section.rfind(".bss", 0) == 0 ||
               section.rfind(".rodata", 0) == 0 || section.rfind(".sdata", 0) == 0 ||
               section.rfind(".sbss", 0) == 0;
    };

    if (name == ".text")
    {
        return -1;
    }
    if (isDataName(name))
    {
        return 1;
    }
    if (name == ".section" && !argument.empty())
    {
        std::string section = argument.substr(0, argument.find_first_of(", \t"));
        if (section.rfind(".text", 0) == 0)
        {
            return -1;
        }
        if (isDataName(section))
        {
            return 1;
        }
    }
    return 0;
}

std::vector<std::string> splitLines(const std::string &source, std::string &lineEnding,
                                    bool &endsWithNewline)
{
    lineEnding = "\n";
    endsWithNewline = !source.empty() && source.back() == '\n';

    std::vector<std::string> lines;
    std::string current;
    for (char c : source)
    {
        if (c == '\n')
        {
            if (!current.empty() && current.back() == '\r')
            {
                current.pop_back();
                lineEnding = "\r\n";
            }
            lines.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty())
    {
        lines.push_back(current);
    }
    return lines;
}

std::vector<Entry> parseEntries(const std::vector<std::string> &lines)
{
    std::vector<Entry> entries;
    std::vector<std::string> pending; // comments and blank lines waiting for an instruction
    bool inData = false;

    auto flushPending = [&]()
    {
        for (const std::string &line : pending)
        {
            Entry entry;
            entry.text = line;
            entries.push_back(std::move(entry));
        }
        pending.clear();
    };

    for (const std::string &line : lines)
    {
        ParsedLine parsed = parseLine(line);

        std::string statementText = line;
        if (!parsed.labels.empty())
        {
            flushPending();
            Entry label;
            const bool isMovableCode = parsed.kind == LineKind::Instruction && !inData;
            if (parsed.body.empty() || !isMovableCode)
            {
                // Label-only lines, "label: .word 1" and data lines are kept exactly as written.
                label.text = line;
                if (parsed.kind == LineKind::Directive)
                {
                    int change = sectionChange(parsed.body);
                    if (change != 0)
                    {
                        inData = (change > 0);
                    }
                }
                entries.push_back(std::move(label));
                continue;
            }
            // "label: instruction" -> put the label on its own line.
            std::string head = line.substr(0, parsed.bodyOffset);
            while (!head.empty() && std::isspace(static_cast<unsigned char>(head.back())))
            {
                head.pop_back();
            }
            label.text = head;
            entries.push_back(std::move(label));
            statementText = "    " + line.substr(parsed.bodyOffset);
        }

        switch (parsed.kind)
        {
            case LineKind::Blank:
            case LineKind::CommentOnly:
                pending.push_back(line);
                break;

            case LineKind::Directive:
            {
                flushPending();
                int change = sectionChange(parsed.body);
                if (change != 0)
                {
                    inData = (change > 0);
                }
                Entry entry;
                entry.text = statementText;
                entries.push_back(std::move(entry));
                break;
            }

            case LineKind::Instruction:
            {
                if (inData)
                {
                    flushPending();
                    Entry entry;
                    entry.text = statementText;
                    entries.push_back(std::move(entry));
                    break;
                }
                Entry entry;
                entry.isInstruction = true;
                entry.leading = std::move(pending);
                pending.clear();
                entry.text = statementText;
                entry.unit = decodeInstruction(parsed.body);
                entries.push_back(std::move(entry));
                break;
            }
        }
    }
    flushPending();
    return entries;
}

void appendOps(std::vector<MicroOp> &stream, const Unit &unit)
{
    stream.insert(stream.end(), unit.ops.begin(), unit.ops.end());
}

/// Records, for every load and store, how many times its base register was changed before it.
void assignBaseVersions(std::vector<Unit> &units)
{
    int version[32] = {0};
    for (size_t i = 0; i < units.size(); ++i)
    {
        Unit &unit = units[i];
        unit.sourceIndex = i;
        if (unit.isLoad || unit.isStore)
        {
            unit.memory.baseVersion = version[unit.memory.baseRegister];
        }
        for (int reg = 1; reg < 32; ++reg)
        {
            if (unit.writes.gpr & (1u << reg))
            {
                ++version[reg];
            }
        }
    }
}

void emitEntry(std::vector<std::string> &output, const Entry &entry)
{
    for (const std::string &line : entry.leading)
    {
        output.push_back(line);
    }
    output.push_back(entry.text);
}

} // namespace

ReorderResult reorderProgram(const std::string &source, PipelineMode mode)
{
    ReorderResult result;
    result.originalText = source;

    std::string lineEnding;
    bool endsWithNewline = false;
    std::vector<std::string> lines = splitLines(source, lineEnding, endsWithNewline);
    std::vector<Entry> entries = parseEntries(lines);

    // Instruction stream in original order, for the "before" count.
    std::vector<MicroOp> originalStream;
    for (const Entry &entry : entries)
    {
        if (entry.isInstruction)
        {
            appendOps(originalStream, entry.unit);
            if (entry.unit.isBarrier)
            {
                ++result.stats.barrierLineCount;
            }
        }
    }
    result.stats.instructionCount = static_cast<int>(originalStream.size());
    result.stats.stallsBefore = countStalls(originalStream, mode);

    std::vector<std::string> output;
    std::vector<MicroOp> outputStream;

    auto flushSegment = [&](std::vector<Entry *> &segment, Entry *terminator)
    {
        if (segment.empty() && terminator == nullptr)
        {
            return;
        }
        ++result.stats.blockCount;

        SegmentInput input;
        for (Entry *entry : segment)
        {
            input.units.push_back(entry->unit);
        }
        assignBaseVersions(input.units);
        if (terminator != nullptr)
        {
            input.hasTerminator = true;
            input.terminator = terminator->unit;
        }
        size_t historyStart =
            outputStream.size() > kHistoryLength ? outputStream.size() - kHistoryLength : 0;
        input.history.assign(outputStream.begin() + static_cast<long>(historyStart),
                             outputStream.end());

        ScheduleResult schedule = scheduleSegment(input, mode);
        if (schedule.usedGreedy)
        {
            result.stats.usedGreedyFallback = true;
        }

        std::vector<size_t> identity(segment.size());
        std::iota(identity.begin(), identity.end(), size_t{0});
        if (schedule.order != identity)
        {
            ++result.stats.blocksChanged;
        }

        for (size_t index : schedule.order)
        {
            emitEntry(output, *segment[index]);
            appendOps(outputStream, segment[index]->unit);
        }
        if (terminator != nullptr)
        {
            emitEntry(output, *terminator);
            appendOps(outputStream, terminator->unit);
        }
        segment.clear();
    };

    std::vector<Entry *> segment;
    for (Entry &entry : entries)
    {
        if (!entry.isInstruction)
        {
            flushSegment(segment, nullptr);
            output.push_back(entry.text);
        }
        else if (entry.unit.isBarrier)
        {
            flushSegment(segment, nullptr);
            emitEntry(output, entry);
            appendOps(outputStream, entry.unit);
        }
        else if (entry.unit.isTerminator)
        {
            flushSegment(segment, &entry);
        }
        else
        {
            segment.push_back(&entry);
        }
    }
    flushSegment(segment, nullptr);

    result.stats.stallsAfter = countStalls(outputStream, mode);

    // Reordering one block can change which instructions precede the next one. If the whole
    // program ended up worse (rare), keep the original instead.
    if (result.stats.stallsAfter >= result.stats.stallsBefore)
    {
        result.stats.stallsAfter = result.stats.stallsBefore;
        result.stats.blocksChanged = 0;
        result.reorderedText = source;
        return result;
    }

    std::string text;
    for (size_t i = 0; i < output.size(); ++i)
    {
        text += output[i];
        if (i + 1 < output.size() || endsWithNewline)
        {
            text += lineEnding;
        }
    }
    result.reorderedText = text;
    return result;
}

} // namespace reorder
} // namespace Kites
