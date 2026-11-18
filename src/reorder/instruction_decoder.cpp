#include "reorder/instruction_decoder.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace Kites
{
namespace reorder
{

namespace
{

// ---------------------------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------------------------

std::string toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string &text)
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

bool isLabelChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$';
}

/// Returns the length of `line` before an unquoted '#' or ';' comment starts.
size_t codeLength(const std::string &line)
{
    size_t pos = line.find('#');
    if (pos != std::string::npos)
    {
        return pos; // Length of code before the comment
    }
    return line.size(); // No comment found, return full line length
}

/// Splits operands at top-level commas, so "4(sp)" and "%lo(x)(a0)" stay in one piece.
std::vector<std::string> splitOperands(const std::string &text)
{
    std::vector<std::string> operands;
    std::string current;
    int depth = 0;
    for (char c : text)
    {
        if (c == '(')
        {
            ++depth;
        }
        else if (c == ')')
        {
            --depth;
        }

        if (c == ',' && depth == 0)
        {
            operands.push_back(trim(current));
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    std::string last = trim(current);
    if (!last.empty() || !operands.empty())
    {
        operands.push_back(last);
    }
    return operands;
}

// ---------------------------------------------------------------------------------------------
// Register names
// ---------------------------------------------------------------------------------------------

const std::unordered_map<std::string, int> &gprNames()
{
    static const std::unordered_map<std::string, int> names = []()
    {
        std::unordered_map<std::string, int> map;
        for (int i = 0; i < 32; ++i)
        {
            map["x" + std::to_string(i)] = i;
        }
        const char *abi[32] = {"zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2",
                               "s0",   "s1", "a0", "a1", "a2",  "a3",  "a4", "a5",
                               "a6",   "a7", "s2", "s3", "s4",  "s5",  "s6", "s7",
                               "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
        for (int i = 0; i < 32; ++i)
        {
            map[abi[i]] = i;
        }
        map["fp"] = 8;
        return map;
    }();
    return names;
}

const std::unordered_map<std::string, int> &fprNames()
{
    static const std::unordered_map<std::string, int> names = []()
    {
        std::unordered_map<std::string, int> map;
        for (int i = 0; i < 32; ++i)
        {
            map["f" + std::to_string(i)] = i;
        }
        for (int i = 0; i < 8; ++i)
        {
            map["ft" + std::to_string(i)] = i;
            map["fa" + std::to_string(i)] = 10 + i;
        }
        map["fs0"] = 8;
        map["fs1"] = 9;
        for (int i = 2; i <= 11; ++i)
        {
            map["fs" + std::to_string(i)] = 16 + i;
        }
        for (int i = 8; i <= 11; ++i)
        {
            map["ft" + std::to_string(i)] = 20 + i;
        }
        return map;
    }();
    return names;
}

// ---------------------------------------------------------------------------------------------
// Instruction formats
// ---------------------------------------------------------------------------------------------

enum class Format
{
    // real instructions
    RType,      // rd, rs1, rs2
    IType,      // rd, rs1, imm
    Load,       // rd, imm(rs1)
    Store,      // rs2, imm(rs1)
    Branch,     // rs1, rs2, label
    Upper,      // rd, imm              (lui, auipc)
    Jal,        // [rd,] label
    Jalr,       // rs1 | rd, rs1, imm | rd, imm(rs1)
    FLoad,      // frd, imm(rs1)
    FStore,     // frs2, imm(rs1)
    FR3,        // frd, frs1, frs2
    FR2,        // frd, frs1
    FR4,        // frd, frs1, frs2, frs3
    FCompare,   // rd, frs1, frs2
    FClassify,  // rd, frs1
    FMoveToInt, // rd, frs1
    FMoveFromInt, // frd, rs1
    FConvert,   // direction decided by the mnemonic
    // pseudo-instructions
    Nop,
    Li,
    La,
    Move,       // rd, rs
    BranchZero, // rs, label
    BranchPair, // rs, rt, label
    Jump,       // label
    JumpReg,    // rs
    Return,
    Call,
    Tail,
    // everything that must not be reordered around
    BarrierFormat
};

const std::unordered_map<std::string, Format> &formatTable()
{
    static const std::unordered_map<std::string, Format> table = []()
    {
        std::unordered_map<std::string, Format> map;

        for (const char *name :
             {"add", "sub", "and", "or", "xor", "sll", "srl", "sra", "slt", "sltu", "addw", "subw",
              "sllw", "srlw", "sraw", "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem",
              "remu", "mulw", "divw", "divuw", "remw", "remuw"})
        {
            map[name] = Format::RType;
        }
        for (const char *name : {"addi", "xori", "ori", "andi", "slli", "srli", "srai", "slti",
                                 "sltiu", "addiw", "slliw", "srliw", "sraiw"})
        {
            map[name] = Format::IType;
        }
        for (const char *name : {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"})
        {
            map[name] = Format::Load;
        }
        for (const char *name : {"sb", "sh", "sw", "sd"})
        {
            map[name] = Format::Store;
        }
        for (const char *name : {"beq", "bne", "blt", "bge", "bltu", "bgeu"})
        {
            map[name] = Format::Branch;
        }
        map["lui"] = Format::Upper;
        map["auipc"] = Format::Upper;
        map["jal"] = Format::Jal;
        map["jalr"] = Format::Jalr;
        map["flw"] = Format::FLoad;
        map["fld"] = Format::FLoad;
        map["fsw"] = Format::FStore;
        map["fsd"] = Format::FStore;

        for (const char *suffix : {".s", ".d"})
        {
            std::string s = suffix;
            for (const char *name : {"fadd", "fsub", "fmul", "fdiv", "fmin", "fmax", "fsgnj",
                                     "fsgnjn", "fsgnjx"})
            {
                map[name + s] = Format::FR3;
            }
            for (const char *name : {"fsqrt", "fmv", "fabs", "fneg"})
            {
                map[name + s] = Format::FR2;
            }
            for (const char *name : {"fmadd", "fmsub", "fnmsub", "fnmadd"})
            {
                map[name + s] = Format::FR4;
            }
            for (const char *name : {"feq", "flt", "fle"})
            {
                map[name + s] = Format::FCompare;
            }
            map["fclass" + s] = Format::FClassify;
        }
        map["fmv.x.w"] = Format::FMoveToInt;
        map["fmv.x.d"] = Format::FMoveToInt;
        map["fmv.w.x"] = Format::FMoveFromInt;
        map["fmv.d.x"] = Format::FMoveFromInt;
        for (const char *name :
             {"fcvt.w.s", "fcvt.wu.s", "fcvt.l.s", "fcvt.lu.s", "fcvt.s.w", "fcvt.s.wu",
              "fcvt.s.l", "fcvt.s.lu", "fcvt.w.d", "fcvt.wu.d", "fcvt.l.d", "fcvt.lu.d",
              "fcvt.d.w", "fcvt.d.wu", "fcvt.d.l", "fcvt.d.lu", "fcvt.s.d", "fcvt.d.s"})
        {
            map[name] = Format::FConvert;
        }

        map["nop"] = Format::Nop;
        map["li"] = Format::Li;
        map["la"] = Format::La;
        for (const char *name :
             {"mv", "not", "neg", "negw", "sext.w", "seqz", "snez", "sltz", "sgtz"})
        {
            map[name] = Format::Move;
        }
        for (const char *name : {"beqz", "bnez", "blez", "bgez", "bltz", "bgtz"})
        {
            map[name] = Format::BranchZero;
        }
        for (const char *name : {"bgt", "ble", "bgtu", "bleu"})
        {
            map[name] = Format::BranchPair;
        }
        map["j"] = Format::Jump;
        map["jr"] = Format::JumpReg;
        map["ret"] = Format::Return;
        map["call"] = Format::Call;
        map["tail"] = Format::Tail;

        // Explicit barriers. Anything unknown is treated the same way.
        for (const char *name :
             {"ecall", "ebreak", "fence", "fence_i", "fence.i", "csrrw", "csrrs", "csrrc",
              "csrrwi", "csrrsi", "csrrci", "csrr", "csrw", "csrs", "csrc"})
        {
            map[name] = Format::BarrierFormat;
        }
        return map;
    }();
    return table;
}

// ---------------------------------------------------------------------------------------------
// Unit construction helpers
// ---------------------------------------------------------------------------------------------

struct MemoryOperand
{
    bool ok = false;
    bool offsetKnown = false;
    int64_t offset = 0;
    int baseRegister = -1;
};

MemoryOperand parseMemoryOperand(const std::string &text)
{
    MemoryOperand result;
    size_t open = text.rfind('(');
    size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open ||
        close != text.size() - 1)
    {
        return result;
    }
    int base = gprIndex(trim(text.substr(open + 1, close - open - 1)));
    if (base < 0)
    {
        return result;
    }
    result.ok = true;
    result.baseRegister = base;

    std::string offsetText = trim(text.substr(0, open));
    if (offsetText.empty())
    {
        result.offsetKnown = true;
        result.offset = 0;
    }
    else
    {
        char *end = nullptr;
        long long value = std::strtoll(offsetText.c_str(), &end, 0);
        if (end != nullptr && *end == '\0')
        {
            result.offsetKnown = true;
            result.offset = value;
        }
    }
    return result;
}

Unit makeBarrier()
{
    Unit unit;
    unit.isBarrier = true;
    unit.ops.push_back(MicroOp{});
    return unit;
}

/// Fills in the aggregated read and write sets of a unit from its micro-ops.
void finalizeUnit(Unit &unit)
{
    RegSet written;
    for (const MicroOp &op : unit.ops)
    {
        RegSet external;
        external.gpr = op.reads.gpr & ~written.gpr;
        external.fpr = op.reads.fpr & ~written.fpr;
        unit.reads.merge(external);
        unit.writes.merge(op.writes);
        written.merge(op.writes);
        if (op.isLoad)
        {
            unit.isLoad = true;
        }
    }
}

MicroOp op(const RegSet &reads, const RegSet &writes, bool isLoad = false)
{
    MicroOp result;
    result.reads = reads;
    result.writes = writes;
    result.isLoad = isLoad;
    return result;
}

int accessSize(const std::string &mnemonic)
{
    if (mnemonic == "lb" || mnemonic == "lbu" || mnemonic == "sb")
    {
        return 1;
    }
    if (mnemonic == "lh" || mnemonic == "lhu" || mnemonic == "sh")
    {
        return 2;
    }
    if (mnemonic == "lw" || mnemonic == "lwu" || mnemonic == "sw" || mnemonic == "flw" ||
        mnemonic == "fsw")
    {
        return 4;
    }
    return 8; // ld, sd, fld, fsd
}

bool parseImmediate(const std::string &text, uint64_t &value)
{
    std::string cleaned = trim(text);
    if (cleaned.empty())
    {
        return false;
    }
    char *end = nullptr;
    if (cleaned[0] == '-')
    {
        long long signedValue = std::strtoll(cleaned.c_str(), &end, 0);
        if (end == nullptr || *end != '\0')
        {
            return false;
        }
        value = static_cast<uint64_t>(signedValue);
        return true;
    }
    unsigned long long unsignedValue = std::strtoull(cleaned.c_str(), &end, 0);
    if (end == nullptr || *end != '\0')
    {
        return false;
    }
    value = static_cast<uint64_t>(unsignedValue);
    return true;
}

bool isIntegerClass(const std::string &type)
{
    return type == "w" || type == "wu" || type == "l" || type == "lu";
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

int gprIndex(const std::string &name)
{
    auto it = gprNames().find(toLower(trim(name)));
    return it == gprNames().end() ? -1 : it->second;
}

int fprIndex(const std::string &name)
{
    auto it = fprNames().find(toLower(trim(name)));
    return it == fprNames().end() ? -1 : it->second;
}

ParsedLine parseLine(const std::string &line)
{
    ParsedLine parsed;
    const std::string code = line.substr(0, codeLength(line));

    size_t pos = 0;
    while (true)
    {
        size_t scan = pos;
        while (scan < code.size() && std::isspace(static_cast<unsigned char>(code[scan])))
        {
            ++scan;
        }
        size_t start = scan;
        while (scan < code.size() && isLabelChar(code[scan]))
        {
            ++scan;
        }
        if (scan > start && scan < code.size() && code[scan] == ':')
        {
            parsed.labels.push_back(code.substr(start, scan - start));
            pos = scan + 1;
            continue;
        }
        break;
    }

    // Offset of the first non-blank character after the labels.
    size_t bodyStart = pos;
    while (bodyStart < code.size() && std::isspace(static_cast<unsigned char>(code[bodyStart])))
    {
        ++bodyStart;
    }
    parsed.bodyOffset = bodyStart;
    parsed.body = trim(code.substr(bodyStart));

    if (parsed.body.empty())
    {
        bool hasComment = trim(line).size() > 0 && parsed.labels.empty();
        parsed.kind = hasComment ? LineKind::CommentOnly : LineKind::Blank;
    }
    else if (parsed.body[0] == '.')
    {
        parsed.kind = LineKind::Directive;
    }
    else
    {
        parsed.kind = LineKind::Instruction;
    }
    return parsed;
}

int liExpansionLength(uint64_t value)
{
    auto fits32 = [](uint64_t v)
    {
        int64_t signedValue = static_cast<int64_t>(v);
        return signedValue == static_cast<int64_t>(static_cast<int32_t>(signedValue));
    };

    int count = 0;
    while (true)
    {
        if (fits32(value))
        {
            int64_t signedValue = static_cast<int64_t>(value);
            if (signedValue >= -2048 && signedValue <= 2047)
            {
                return count + 1;
            }
            int64_t upper = (signedValue + 2048) >> 12;
            int64_t lower = signedValue - (upper << 12);
            return count + 1 + (lower != 0 ? 1 : 0);
        }

        int64_t lo12 = static_cast<int64_t>(value & 0xFFF);
        if (lo12 & 0x800)
        {
            lo12 -= 0x1000;
        }
        value -= static_cast<uint64_t>(lo12);

        int shift = 0;
        uint64_t probe = value;
        while ((probe & 1) == 0 && shift < 64)
        {
            probe >>= 1;
            ++shift;
        }
        value >>= shift;

        count += 1 + (lo12 != 0 ? 1 : 0); // slli, plus addi when there is a low part
    }
}

Unit decodeInstruction(const std::string &body)
{
    // Split "mnemonic operands".
    size_t split = 0;
    while (split < body.size() && !std::isspace(static_cast<unsigned char>(body[split])))
    {
        ++split;
    }
    const std::string mnemonic = toLower(body.substr(0, split));
    const std::vector<std::string> operands = splitOperands(body.substr(split));

    auto formatIt = formatTable().find(mnemonic);
    if (formatIt == formatTable().end() || formatIt->second == Format::BarrierFormat)
    {
        return makeBarrier();
    }
    const Format format = formatIt->second;

    auto gpr = [&](size_t index) -> int
    { return index < operands.size() ? gprIndex(operands[index]) : -1; };
    auto fpr = [&](size_t index) -> int
    { return index < operands.size() ? fprIndex(operands[index]) : -1; };

    Unit unit;

    switch (format)
    {
        case Format::RType:
        {
            int rd = gpr(0), rs1 = gpr(1), rs2 = gpr(2);
            if (rd < 0 || rs1 < 0 || rs2 < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addGpr(rs1);
            reads.addGpr(rs2);
            writes.addGpr(rd);
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::IType:
        {
            int rd = gpr(0), rs1 = gpr(1);
            if (rd < 0 || rs1 < 0 || operands.size() < 3)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addGpr(rs1);
            writes.addGpr(rd);
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::Load:
        case Format::FLoad:
        {
            if (operands.size() < 2)
            {
                return makeBarrier();
            }
            MemoryOperand memory = parseMemoryOperand(operands[1]);
            int rd = (format == Format::Load) ? gpr(0) : fpr(0);
            if (!memory.ok || rd < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addGpr(memory.baseRegister);
            if (format == Format::Load)
            {
                writes.addGpr(rd);
            }
            else
            {
                writes.addFpr(rd);
            }
            unit.ops.push_back(op(reads, writes, true));
            unit.memory.valid = memory.offsetKnown;
            unit.memory.baseRegister = memory.baseRegister;
            unit.memory.offset = memory.offset;
            unit.memory.size = accessSize(mnemonic);
            break;
        }
        case Format::Store:
        case Format::FStore:
        {
            if (operands.size() < 2)
            {
                return makeBarrier();
            }
            MemoryOperand memory = parseMemoryOperand(operands[1]);
            int source = (format == Format::Store) ? gpr(0) : fpr(0);
            if (!memory.ok || source < 0)
            {
                return makeBarrier();
            }
            RegSet reads;
            reads.addGpr(memory.baseRegister);
            if (format == Format::Store)
            {
                reads.addGpr(source);
            }
            else
            {
                reads.addFpr(source);
            }
            unit.ops.push_back(op(reads, RegSet{}));
            unit.isStore = true;
            unit.memory.valid = memory.offsetKnown;
            unit.memory.baseRegister = memory.baseRegister;
            unit.memory.offset = memory.offset;
            unit.memory.size = accessSize(mnemonic);
            break;
        }
        case Format::Branch:
        {
            int rs1 = gpr(0), rs2 = gpr(1);
            if (rs1 < 0 || rs2 < 0 || operands.size() < 3)
            {
                return makeBarrier();
            }
            RegSet reads;
            reads.addGpr(rs1);
            reads.addGpr(rs2);
            unit.ops.push_back(op(reads, RegSet{}));
            unit.isTerminator = true;
            break;
        }
        case Format::BranchZero:
        {
            int rs = gpr(0);
            if (rs < 0 || operands.size() < 2)
            {
                return makeBarrier();
            }
            RegSet reads;
            reads.addGpr(rs);
            unit.ops.push_back(op(reads, RegSet{}));
            unit.isTerminator = true;
            break;
        }
        case Format::BranchPair:
        {
            int rs = gpr(0), rt = gpr(1);
            if (rs < 0 || rt < 0 || operands.size() < 3)
            {
                return makeBarrier();
            }
            RegSet reads;
            reads.addGpr(rs);
            reads.addGpr(rt);
            unit.ops.push_back(op(reads, RegSet{}));
            unit.isTerminator = true;
            break;
        }
        case Format::Upper:
        {
            int rd = gpr(0);
            if (rd < 0 || operands.size() < 2)
            {
                return makeBarrier();
            }
            RegSet writes;
            writes.addGpr(rd);
            unit.ops.push_back(op(RegSet{}, writes));
            break;
        }
        case Format::Jal:
        {
            int rd = 1; // jal label  ==  jal ra, label
            if (operands.size() >= 2)
            {
                rd = gpr(0);
            }
            else if (operands.size() != 1)
            {
                return makeBarrier();
            }
            if (rd < 0)
            {
                return makeBarrier();
            }
            RegSet writes;
            writes.addGpr(rd);
            unit.ops.push_back(op(RegSet{}, writes));
            unit.isTerminator = true;
            break;
        }
        case Format::Jalr:
        {
            int rd = 1;
            int rs1 = -1;
            if (operands.size() == 1)
            {
                rs1 = gpr(0);
            }
            else if (operands.size() == 2)
            {
                MemoryOperand memory = parseMemoryOperand(operands[1]);
                rd = gpr(0);
                rs1 = memory.ok ? memory.baseRegister : -1;
            }
            else if (operands.size() == 3)
            {
                rd = gpr(0);
                rs1 = gpr(1);
            }
            if (rd < 0 || rs1 < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addGpr(rs1);
            writes.addGpr(rd);
            unit.ops.push_back(op(reads, writes));
            unit.isTerminator = true;
            break;
        }
        case Format::FR3:
        case Format::FR2:
        case Format::FR4:
        {
            size_t sourceCount = (format == Format::FR3) ? 2 : (format == Format::FR2 ? 1 : 3);
            int frd = fpr(0);
            if (frd < 0 || operands.size() < sourceCount + 1)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            for (size_t i = 1; i <= sourceCount; ++i)
            {
                int source = fpr(i);
                if (source < 0)
                {
                    return makeBarrier();
                }
                reads.addFpr(source);
            }
            writes.addFpr(frd);
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::FCompare:
        {
            int rd = gpr(0), a = fpr(1), b = fpr(2);
            if (rd < 0 || a < 0 || b < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addFpr(a);
            reads.addFpr(b);
            writes.addGpr(rd);
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::FClassify:
        case Format::FMoveToInt:
        {
            int rd = gpr(0), a = fpr(1);
            if (rd < 0 || a < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addFpr(a);
            writes.addGpr(rd);
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::FMoveFromInt:
        {
            int frd = fpr(0), rs1 = gpr(1);
            if (frd < 0 || rs1 < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addGpr(rs1);
            writes.addFpr(frd);
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::FConvert:
        {
            // fcvt.<destination>.<source>
            size_t first = mnemonic.find('.');
            size_t second = mnemonic.find('.', first + 1);
            if (first == std::string::npos || second == std::string::npos)
            {
                return makeBarrier();
            }
            std::string destinationType = mnemonic.substr(first + 1, second - first - 1);
            std::string sourceType = mnemonic.substr(second + 1);
            bool destinationIsInt = isIntegerClass(destinationType);
            bool sourceIsInt = isIntegerClass(sourceType);

            int destination = destinationIsInt ? gpr(0) : fpr(0);
            int source = sourceIsInt ? gpr(1) : fpr(1);
            if (destination < 0 || source < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            if (sourceIsInt)
            {
                reads.addGpr(source);
            }
            else
            {
                reads.addFpr(source);
            }
            if (destinationIsInt)
            {
                writes.addGpr(destination);
            }
            else
            {
                writes.addFpr(destination);
            }
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::Nop:
        {
            unit.ops.push_back(MicroOp{});
            break;
        }
        case Format::Li:
        {
            int rd = gpr(0);
            uint64_t value = 0;
            if (rd < 0 || operands.size() < 2 || !parseImmediate(operands[1], value))
            {
                return makeBarrier();
            }
            int length = liExpansionLength(value);
            RegSet rdSet;
            rdSet.addGpr(rd);
            for (int i = 0; i < length; ++i)
            {
                // The first instruction only writes rd, the rest build on it.
                unit.ops.push_back(op(i == 0 ? RegSet{} : rdSet, rdSet));
            }
            break;
        }
        case Format::La:
        {
            int rd = gpr(0);
            if (rd < 0 || operands.size() < 2)
            {
                return makeBarrier();
            }
            RegSet rdSet;
            rdSet.addGpr(rd);
            unit.ops.push_back(op(RegSet{}, rdSet));  // auipc rd, hi
            unit.ops.push_back(op(rdSet, rdSet));     // addi  rd, rd, lo
            break;
        }
        case Format::Move:
        {
            int rd = gpr(0), rs = gpr(1);
            if (rd < 0 || rs < 0)
            {
                return makeBarrier();
            }
            RegSet reads, writes;
            reads.addGpr(rs);
            writes.addGpr(rd);
            unit.ops.push_back(op(reads, writes));
            break;
        }
        case Format::Jump:
        {
            unit.ops.push_back(MicroOp{});
            unit.isTerminator = true;
            break;
        }
        case Format::JumpReg:
        {
            int rs = gpr(0);
            if (rs < 0)
            {
                return makeBarrier();
            }
            RegSet reads;
            reads.addGpr(rs);
            unit.ops.push_back(op(reads, RegSet{}));
            unit.isTerminator = true;
            break;
        }
        case Format::Return:
        {
            RegSet reads;
            reads.addGpr(1); // ra
            unit.ops.push_back(op(reads, RegSet{}));
            unit.isTerminator = true;
            break;
        }
        case Format::Call:
        {
            RegSet ra;
            ra.addGpr(1);
            unit.ops.push_back(op(RegSet{}, ra)); // auipc ra, hi
            unit.ops.push_back(op(ra, ra));       // jalr  ra, lo(ra)
            unit.isTerminator = true;
            break;
        }
        case Format::Tail:
        {
            RegSet t1;
            t1.addGpr(6);
            unit.ops.push_back(op(RegSet{}, t1)); // auipc t1, hi
            unit.ops.push_back(op(t1, RegSet{})); // jalr  x0, lo(t1)
            unit.isTerminator = true;
            break;
        }
        case Format::BarrierFormat:
            return makeBarrier();
    }

    finalizeUnit(unit);
    return unit;
}

} // namespace reorder
} // namespace Kites
