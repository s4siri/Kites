#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "reorder/code_reorderer.h"
#include "reorder/hazard_model.h"
#include "reorder/instruction_decoder.h"
#include "reorder/scheduler.h"

using namespace Kites::reorder;

namespace
{

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

std::vector<std::string> splitLinesForTest(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        lines.push_back(line);
    }
    return lines;
}

/// Index of the first line containing `needle`, or -1.
int lineOf(const std::string &text, const std::string &needle)
{
    std::vector<std::string> lines = splitLinesForTest(text);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].find(needle) != std::string::npos)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int stallsFor(const std::string &program, PipelineMode mode)
{
    return reorderProgram(program, mode).stats.stallsBefore;
}

// ---------------------------------------------------------------------------------------------
// A tiny RV64 interpreter used to prove that reordering never changes program behaviour.
// It understands only the instructions produced by the random program generator below.
// ---------------------------------------------------------------------------------------------

struct Machine
{
    uint64_t regs[32] = {0};
    std::map<uint64_t, uint8_t> memory;

    bool operator==(const Machine &other) const
    {
        for (int i = 0; i < 32; ++i)
        {
            if (regs[i] != other.regs[i])
            {
                return false;
            }
        }
        return memory == other.memory;
    }
};

std::vector<std::string> tokens(const std::string &line)
{
    std::vector<std::string> result;
    std::string current;
    for (char c : line)
    {
        if (c == ' ' || c == ',' || c == '(' || c == ')' || c == '\t')
        {
            if (!current.empty())
            {
                result.push_back(current);
                current.clear();
            }
        }
        else if (c == '#')
        {
            break;
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty())
    {
        result.push_back(current);
    }
    return result;
}

void store(Machine &m, uint64_t address, uint64_t value, int size)
{
    for (int i = 0; i < size; ++i)
    {
        m.memory[address + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint64_t load(const Machine &m, uint64_t address, int size, bool isSigned)
{
    uint64_t value = 0;
    for (int i = 0; i < size; ++i)
    {
        auto it = m.memory.find(address + i);
        uint64_t byte = (it == m.memory.end()) ? 0 : it->second;
        value |= byte << (8 * i);
    }
    if (isSigned && size < 8)
    {
        uint64_t signBit = uint64_t{1} << (size * 8 - 1);
        if (value & signBit)
        {
            value |= ~((signBit << 1) - 1);
        }
    }
    return value;
}

void run(Machine &m, const std::string &program)
{
    for (const std::string &line : splitLinesForTest(program))
    {
        std::vector<std::string> t = tokens(line);
        if (t.empty() || t[0].back() == ':')
        {
            continue;
        }
        const std::string &op = t[0];
        auto reg = [&](size_t i) { return gprIndex(t[i]); };
        auto imm = [&](size_t i) { return static_cast<int64_t>(std::stoll(t[i], nullptr, 0)); };
        auto set = [&](int rd, uint64_t value)
        {
            if (rd != 0)
            {
                m.regs[rd] = value;
            }
        };

        if (op == "li")
        {
            long long value = std::stoll(t[2], nullptr, 0);
            set(reg(1), static_cast<uint64_t>(value));
        }
        else if (op == "mv")
        {
            set(reg(1), m.regs[reg(2)]);
        }
        else if (op == "lui")
        {
            set(reg(1), static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(imm(2) << 12))));
        }
        else if (op == "addi")
        {
            set(reg(1), m.regs[reg(2)] + static_cast<uint64_t>(imm(3)));
        }
        else if (op == "andi")
        {
            set(reg(1), m.regs[reg(2)] & static_cast<uint64_t>(imm(3)));
        }
        else if (op == "ori")
        {
            set(reg(1), m.regs[reg(2)] | static_cast<uint64_t>(imm(3)));
        }
        else if (op == "xori")
        {
            set(reg(1), m.regs[reg(2)] ^ static_cast<uint64_t>(imm(3)));
        }
        else if (op == "slli")
        {
            set(reg(1), m.regs[reg(2)] << (imm(3) & 63));
        }
        else if (op == "srli")
        {
            set(reg(1), m.regs[reg(2)] >> (imm(3) & 63));
        }
        else if (op == "add")
        {
            set(reg(1), m.regs[reg(2)] + m.regs[reg(3)]);
        }
        else if (op == "sub")
        {
            set(reg(1), m.regs[reg(2)] - m.regs[reg(3)]);
        }
        else if (op == "and")
        {
            set(reg(1), m.regs[reg(2)] & m.regs[reg(3)]);
        }
        else if (op == "or")
        {
            set(reg(1), m.regs[reg(2)] | m.regs[reg(3)]);
        }
        else if (op == "xor")
        {
            set(reg(1), m.regs[reg(2)] ^ m.regs[reg(3)]);
        }
        else if (op == "mul")
        {
            set(reg(1), m.regs[reg(2)] * m.regs[reg(3)]);
        }
        else if (op == "slt")
        {
            set(reg(1), static_cast<int64_t>(m.regs[reg(2)]) < static_cast<int64_t>(m.regs[reg(3)]));
        }
        else if (op == "lb" || op == "lbu" || op == "lh" || op == "lhu" || op == "lw" ||
                 op == "lwu" || op == "ld")
        {
            int size = (op[1] == 'b') ? 1 : (op[1] == 'h') ? 2 : (op[1] == 'w') ? 4 : 8;
            bool isSigned = (op == "lb" || op == "lh" || op == "lw" || op == "ld");
            uint64_t address = m.regs[reg(3)] + static_cast<uint64_t>(imm(2));
            set(reg(1), load(m, address, size, isSigned));
        }
        else if (op == "sb" || op == "sh" || op == "sw" || op == "sd")
        {
            int size = (op[1] == 'b') ? 1 : (op[1] == 'h') ? 2 : (op[1] == 'w') ? 4 : 8;
            uint64_t address = m.regs[reg(3)] + static_cast<uint64_t>(imm(2));
            store(m, address, m.regs[reg(1)], size);
        }
        else
        {
            FAIL() << "interpreter does not know: " << line;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Random program generator
// ---------------------------------------------------------------------------------------------

std::string randomProgram(std::mt19937 &rng, int length)
{
    // sp (x2) is a fixed base; t2 is a movable base so the aliasing rules are exercised.
    static const char *kRegs[] = {"a0", "a1", "a2", "a3", "a4", "a5", "t0", "t1", "t2"};
    auto pick = [&]() { return std::string(kRegs[rng() % 9]); };
    auto offset = [&]() { return static_cast<int>(rng() % 24); }; // unaligned on purpose

    std::ostringstream out;
    out << "main:\n";
    for (int i = 0; i < length; ++i)
    {
        switch (rng() % 14)
        {
            case 0: out << "    add " << pick() << ", " << pick() << ", " << pick() << "\n"; break;
            case 1: out << "    sub " << pick() << ", " << pick() << ", " << pick() << "\n"; break;
            case 2: out << "    xor " << pick() << ", " << pick() << ", " << pick() << "\n"; break;
            case 3: out << "    mul " << pick() << ", " << pick() << ", " << pick() << "\n"; break;
            case 4: out << "    addi " << pick() << ", " << pick() << ", " << (int(rng() % 200) - 100) << "\n"; break;
            case 5: out << "    slli " << pick() << ", " << pick() << ", " << rng() % 8 << "\n"; break;
            case 6: out << "    li " << pick() << ", " << (int(rng() % 40) - 20) << "\n"; break;
            case 7: out << "    li " << pick() << ", " << (0x12345678 + int(rng() % 1000)) << "\n"; break;
            case 8: out << "    mv " << pick() << ", " << pick() << "\n"; break;
            case 9: out << "    lw " << pick() << ", " << offset() << "(sp)\n"; break;
            case 10: out << "    ld " << pick() << ", " << offset() << "(t2)\n"; break;
            case 11: out << "    sw " << pick() << ", " << offset() << "(sp)\n"; break;
            case 12: out << "    sd " << pick() << ", " << offset() << "(t2)\n"; break;
            case 13: out << "    sb " << pick() << ", " << offset() << "(sp)\n"; break;
        }
    }
    return out.str();
}

Machine freshMachine()
{
    Machine m;
    for (int i = 1; i < 32; ++i)
    {
        m.regs[i] = 0x1111111111111111ULL * (i % 15 + 1) + i;
    }
    m.regs[2] = 0x1000; // sp
    m.regs[7] = 0x2000; // t2, separate region from sp so only same-base accesses overlap
    return m;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------------------------

TEST(ReorderDecoder, RegisterNames)
{
    EXPECT_EQ(gprIndex("x0"), 0);
    EXPECT_EQ(gprIndex("zero"), 0);
    EXPECT_EQ(gprIndex("ra"), 1);
    EXPECT_EQ(gprIndex("sp"), 2);
    EXPECT_EQ(gprIndex("t0"), 5);
    EXPECT_EQ(gprIndex("fp"), 8);
    EXPECT_EQ(gprIndex("s0"), 8);
    EXPECT_EQ(gprIndex("a0"), 10);
    EXPECT_EQ(gprIndex("a7"), 17);
    EXPECT_EQ(gprIndex("s11"), 27);
    EXPECT_EQ(gprIndex("t6"), 31);
    EXPECT_EQ(gprIndex("x32"), -1);
    EXPECT_EQ(gprIndex("fa0"), -1);

    EXPECT_EQ(fprIndex("f0"), 0);
    EXPECT_EQ(fprIndex("ft7"), 7);
    EXPECT_EQ(fprIndex("fs0"), 8);
    EXPECT_EQ(fprIndex("fa0"), 10);
    EXPECT_EQ(fprIndex("fs2"), 18);
    EXPECT_EQ(fprIndex("fs11"), 27);
    EXPECT_EQ(fprIndex("ft8"), 28);
    EXPECT_EQ(fprIndex("ft11"), 31);
    EXPECT_EQ(fprIndex("a0"), -1);
}

TEST(ReorderDecoder, LiExpansionLengthMatchesAssembler)
{
    EXPECT_EQ(liExpansionLength(0), 1);
    EXPECT_EQ(liExpansionLength(5), 1);
    EXPECT_EQ(liExpansionLength(static_cast<uint64_t>(-2048)), 1);
    EXPECT_EQ(liExpansionLength(2047), 1);
    EXPECT_EQ(liExpansionLength(2048), 2);         // lui + addi
    EXPECT_EQ(liExpansionLength(0x12345000), 1);   // lui only
    EXPECT_EQ(liExpansionLength(0x12345678), 2);   // lui + addi
    EXPECT_GT(liExpansionLength(0x123456789ABCDEF0ULL), 2);
}

TEST(ReorderDecoder, ParsesLabelsAndComments)
{
    ParsedLine p = parseLine("loop:  addi a0, a0, 1   # count");
    ASSERT_EQ(p.labels.size(), 1u);
    EXPECT_EQ(p.labels[0], "loop");
    EXPECT_EQ(p.body, "addi a0, a0, 1");
    EXPECT_EQ(p.kind, LineKind::Instruction);

    EXPECT_EQ(parseLine("   # only a comment").kind, LineKind::CommentOnly);
    EXPECT_EQ(parseLine("").kind, LineKind::Blank);
    EXPECT_EQ(parseLine("  .word 5, 6  ; two words").kind, LineKind::Directive);
    EXPECT_EQ(parseLine(".L1:").kind, LineKind::Blank);
    EXPECT_EQ(parseLine(".L1:").labels.size(), 1u);
    EXPECT_EQ(parseLine("a: b: nop").labels.size(), 2u);
    EXPECT_EQ(parseLine(".asciz \"a#b\"").body, ".asciz \"a#b\"");
}

TEST(ReorderDecoder, ReadWriteSets)
{
    Unit add = decodeInstruction("add a0, a1, a2");
    EXPECT_EQ(add.writes.gpr, 1u << 10);
    EXPECT_EQ(add.reads.gpr, (1u << 11) | (1u << 12));

    Unit toZero = decodeInstruction("addi x0, a1, 1");
    EXPECT_TRUE(toZero.writes.empty());

    Unit lw = decodeInstruction("lw a0, 8(sp)");
    EXPECT_TRUE(lw.isLoad);
    EXPECT_TRUE(lw.memory.valid);
    EXPECT_EQ(lw.memory.baseRegister, 2);
    EXPECT_EQ(lw.memory.offset, 8);
    EXPECT_EQ(lw.memory.size, 4);
    EXPECT_EQ(lw.reads.gpr, 1u << 2);

    Unit sd = decodeInstruction("sd a1, -16(sp)");
    EXPECT_TRUE(sd.isStore);
    EXPECT_EQ(sd.memory.offset, -16);
    EXPECT_EQ(sd.memory.size, 8);
    EXPECT_EQ(sd.reads.gpr, (1u << 11) | (1u << 2));
    EXPECT_TRUE(sd.writes.empty());

    Unit beq = decodeInstruction("beq a0, a1, done");
    EXPECT_TRUE(beq.isTerminator);
    EXPECT_EQ(beq.reads.gpr, (1u << 10) | (1u << 11));

    Unit jal = decodeInstruction("jal done");
    EXPECT_TRUE(jal.isTerminator);
    EXPECT_EQ(jal.writes.gpr, 1u << 1);

    Unit li = decodeInstruction("li a0, 0x12345678");
    EXPECT_EQ(li.ops.size(), 2u);
    EXPECT_TRUE(li.reads.empty()); // second op reads what the first wrote, nothing external
    EXPECT_EQ(li.writes.gpr, 1u << 10);

    Unit la = decodeInstruction("la t0, data");
    EXPECT_EQ(la.ops.size(), 2u);

    Unit ret = decodeInstruction("ret");
    EXPECT_TRUE(ret.isTerminator);
    EXPECT_EQ(ret.reads.gpr, 1u << 1);
}

TEST(ReorderDecoder, FloatingPointSets)
{
    Unit fadd = decodeInstruction("fadd.s fa0, fa1, fa2");
    EXPECT_EQ(fadd.writes.fpr, 1u << 10);
    EXPECT_EQ(fadd.reads.fpr, (1u << 11) | (1u << 12));
    EXPECT_EQ(fadd.reads.gpr, 0u);

    Unit fmadd = decodeInstruction("fmadd.d f1, f2, f3, f4, rne");
    EXPECT_EQ(fmadd.reads.fpr, (1u << 2) | (1u << 3) | (1u << 4));

    Unit flw = decodeInstruction("flw ft0, 4(a0)");
    EXPECT_TRUE(flw.isLoad);
    EXPECT_EQ(flw.writes.fpr, 1u);
    EXPECT_EQ(flw.reads.gpr, 1u << 10);

    Unit fsw = decodeInstruction("fsw ft0, 4(a0)");
    EXPECT_TRUE(fsw.isStore);
    EXPECT_EQ(fsw.reads.fpr, 1u);

    Unit feq = decodeInstruction("feq.s a0, fa0, fa1");
    EXPECT_EQ(feq.writes.gpr, 1u << 10);
    EXPECT_EQ(feq.reads.fpr, (1u << 10) | (1u << 11));

    Unit toInt = decodeInstruction("fcvt.w.s a0, fa0");
    EXPECT_EQ(toInt.writes.gpr, 1u << 10);
    EXPECT_EQ(toInt.reads.fpr, 1u << 10);

    Unit toFp = decodeInstruction("fcvt.s.w fa0, a0");
    EXPECT_EQ(toFp.writes.fpr, 1u << 10);
    EXPECT_EQ(toFp.reads.gpr, 1u << 10);

    Unit fmvXW = decodeInstruction("fmv.x.w a0, fa0");
    EXPECT_EQ(fmvXW.writes.gpr, 1u << 10);
    Unit fmvWX = decodeInstruction("fmv.w.x fa0, a0");
    EXPECT_EQ(fmvWX.writes.fpr, 1u << 10);

    // f0 is a real register (unlike x0).
    Unit f0 = decodeInstruction("fadd.s f0, f0, f1");
    EXPECT_EQ(f0.writes.fpr, 1u);
}

TEST(ReorderDecoder, UnknownInstructionsAreBarriers)
{
    EXPECT_TRUE(decodeInstruction("ecall").isBarrier);
    EXPECT_TRUE(decodeInstruction("csrrw a0, mstatus, a1").isBarrier);
    EXPECT_TRUE(decodeInstruction("fence").isBarrier);
    EXPECT_TRUE(decodeInstruction("mycustompseudo a0, a1").isBarrier);
    EXPECT_TRUE(decodeInstruction("add a0, a1").isBarrier);       // missing operand
    EXPECT_TRUE(decodeInstruction("add a0, a1, 5").isBarrier);    // not a register
}

// ---------------------------------------------------------------------------------------------
// Hazard model (mirrors check_data_hazard in rv5s_hdu.cpp)
// ---------------------------------------------------------------------------------------------

TEST(ReorderHazardModel, NoForwardingDistances)
{
    // distance 1 -> 2 stalls, distance 2 -> 1 stall, distance 3 -> none
    EXPECT_EQ(stallsFor("add a0, a1, a2\nadd a3, a0, a4\n", PipelineMode::NoForwarding), 2);
    EXPECT_EQ(stallsFor("add a0, a1, a2\nadd t0, t1, t2\nadd a3, a0, a4\n",
                        PipelineMode::NoForwarding),
              1);
    EXPECT_EQ(stallsFor("add a0, a1, a2\nadd t0, t1, t2\nadd t3, t4, t5\nadd a3, a0, a4\n",
                        PipelineMode::NoForwarding),
              0);
}

TEST(ReorderHazardModel, ForwardingOnlyStallsOnLoadUse)
{
    EXPECT_EQ(stallsFor("add a0, a1, a2\nadd a3, a0, a4\n", PipelineMode::Forwarding), 0);
    EXPECT_EQ(stallsFor("lw a0, 0(sp)\nadd a3, a0, a4\n", PipelineMode::Forwarding), 1);
    EXPECT_EQ(stallsFor("lw a0, 0(sp)\nadd t0, t1, t2\nadd a3, a0, a4\n", PipelineMode::Forwarding),
              0);
    // Store data counts as a use as well.
    EXPECT_EQ(stallsFor("lw a0, 0(sp)\nsw a0, 8(sp)\n", PipelineMode::Forwarding), 1);
}

TEST(ReorderHazardModel, StallsPushLaterConsumersBack)
{
    // Once the first consumer has waited 2 cycles, its own consumer is already far enough away.
    std::string program = "add a0, a1, a2\nadd a3, a0, a4\nadd a5, a3, a4\n";
    // a0 -> a3: 2 stalls; a3 -> a5 is again back-to-back: 2 more.
    EXPECT_EQ(stallsFor(program, PipelineMode::NoForwarding), 4);
}

TEST(ReorderHazardModel, X0IsNeverAHazard)
{
    EXPECT_EQ(stallsFor("addi x0, a1, 1\nadd a3, x0, a4\n", PipelineMode::NoForwarding), 0);
}

TEST(ReorderHazardModel, FloatingPointHazards)
{
    EXPECT_EQ(stallsFor("fadd.s f0, f1, f2\nfadd.s f3, f0, f2\n", PipelineMode::NoForwarding), 2);
    EXPECT_EQ(stallsFor("flw ft0, 0(a0)\nfadd.s ft1, ft0, ft0\n", PipelineMode::Forwarding), 1);
    EXPECT_EQ(stallsFor("fadd.s ft0, ft1, ft2\nfadd.s ft3, ft0, ft0\n", PipelineMode::Forwarding),
              0);
    // Integer and floating point registers with the same number are different registers.
    EXPECT_EQ(stallsFor("add a0, a1, a2\nfadd.s fa0, fa0, fa0\n", PipelineMode::NoForwarding), 0);
}

// ---------------------------------------------------------------------------------------------
// Reordering behaviour
// ---------------------------------------------------------------------------------------------

TEST(ReorderEngine, HidesLoadUseWithForwarding)
{
    const std::string program =
        "    lw a1, 0(a0)\n"
        "    add a2, a1, a1\n"
        "    lw a3, 4(a0)\n"
        "    add a4, a3, a3\n";

    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    EXPECT_EQ(result.stats.stallsBefore, 2);
    EXPECT_EQ(result.stats.stallsAfter, 0);
    EXPECT_EQ(result.stats.blocksChanged, 1);
    EXPECT_LT(lineOf(result.reorderedText, "lw a3"), lineOf(result.reorderedText, "add a2"));
}

TEST(ReorderEngine, FillsGapsWithoutForwarding)
{
    const std::string program =
        "    addi t0, zero, 1\n"
        "    addi t1, t0, 1\n"   // depends on t0 (2 stalls)
        "    addi t2, zero, 2\n" // independent filler
        "    addi t3, zero, 3\n" // independent filler
        "    addi t4, zero, 4\n";

    ReorderResult result = reorderProgram(program, PipelineMode::NoForwarding);
    EXPECT_EQ(result.stats.stallsBefore, 2);
    EXPECT_EQ(result.stats.stallsAfter, 0);
    // The dependent instruction must still come after its producer.
    EXPECT_LT(lineOf(result.reorderedText, "addi t0"), lineOf(result.reorderedText, "addi t1"));
}

TEST(ReorderEngine, ModesProduceDifferentSchedules)
{
    // add -> add is free with forwarding but stalls without it.
    const std::string program =
        "    add a0, a1, a2\n"
        "    add a3, a0, a2\n"
        "    add a4, a5, a5\n"
        "    add t0, a5, a5\n";

    ReorderResult withForwarding = reorderProgram(program, PipelineMode::Forwarding);
    ReorderResult withoutForwarding = reorderProgram(program, PipelineMode::NoForwarding);

    EXPECT_EQ(withForwarding.stats.stallsBefore, 0);
    EXPECT_EQ(withForwarding.reorderedText, program); // nothing to improve
    EXPECT_EQ(withoutForwarding.stats.stallsBefore, 2);
    EXPECT_EQ(withoutForwarding.stats.stallsAfter, 0);
    EXPECT_NE(withoutForwarding.reorderedText, program);
}

TEST(ReorderEngine, KeepsDependentInstructionsOrdered)
{
    // WAW + WAR + RAW chain, nothing can move even though it stalls.
    const std::string program =
        "    li a0, 1\n"
        "    add a1, a0, a0\n"
        "    li a0, 2\n"
        "    add a2, a0, a1\n";
    ReorderResult result = reorderProgram(program, PipelineMode::NoForwarding);
    EXPECT_EQ(result.reorderedText, program);
}

TEST(ReorderEngine, StoreLoadOrderingIsRespected)
{
    // The load reads what the store wrote, so they cannot swap even though registers are free.
    const std::string program =
        "    sw a1, 0(sp)\n"
        "    lw a2, 0(sp)\n"
        "    add a3, a2, a2\n"
        "    addi a4, zero, 1\n"
        "    addi a5, zero, 2\n";
    ReorderResult result = reorderProgram(program, PipelineMode::NoForwarding);
    EXPECT_LT(lineOf(result.reorderedText, "sw a1"), lineOf(result.reorderedText, "lw a2"));
    EXPECT_LT(lineOf(result.reorderedText, "lw a2"), lineOf(result.reorderedText, "add a3"));
}

TEST(ReorderEngine, DisjointMemoryAccessesMayBeReordered)
{
    // Same base register, non-overlapping offsets: the store may move below the load.
    const std::string program =
        "    lw a1, 0(sp)\n"
        "    add a2, a1, a1\n"
        "    sw a3, 8(sp)\n"
        "    lw a4, 16(sp)\n"
        "    add a5, a4, a4\n";
    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    EXPECT_EQ(result.stats.stallsAfter, 0);
}

TEST(ReorderEngine, ChangedBaseRegisterBlocksDisambiguation)
{
    // sp changes between the store and the load, so the offsets are not comparable.
    const std::string program =
        "    sw a1, 0(t0)\n"
        "    addi t0, t0, 4\n"
        "    lw a2, -4(t0)\n"   // really the same address as the store
        "    add a3, a2, a2\n"
        "    addi a4, zero, 1\n"
        "    addi a5, zero, 2\n";
    ReorderResult result = reorderProgram(program, PipelineMode::NoForwarding);
    EXPECT_LT(lineOf(result.reorderedText, "sw a1"), lineOf(result.reorderedText, "lw a2"));
}

TEST(ReorderEngine, NothingCrossesLabelsOrBranches)
{
    const std::string program =
        "start:\n"
        "    lw a1, 0(a0)\n"
        "    add a2, a1, a1\n"
        "    lw a3, 4(a0)\n"
        "    add a4, a3, a3\n"
        "    beq a2, a4, done\n"
        "    addi a5, zero, 1\n"
        "done:\n"
        "    addi a6, zero, 2\n";

    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    int start = lineOf(result.reorderedText, "start:");
    int branch = lineOf(result.reorderedText, "beq");
    int done = lineOf(result.reorderedText, "done:");
    EXPECT_EQ(start, 0);
    EXPECT_LT(start, lineOf(result.reorderedText, "lw a1"));
    EXPECT_LT(lineOf(result.reorderedText, "lw a1"), branch);
    EXPECT_LT(lineOf(result.reorderedText, "add a4"), branch);
    EXPECT_LT(branch, lineOf(result.reorderedText, "addi a5"));
    EXPECT_LT(lineOf(result.reorderedText, "addi a5"), done);
    EXPECT_LT(done, lineOf(result.reorderedText, "addi a6"));
}

TEST(ReorderEngine, BarriersStayPut)
{
    const std::string program =
        "    lw a1, 0(a0)\n"
        "    add a2, a1, a1\n"
        "    ecall\n"
        "    lw a3, 4(a0)\n"
        "    add a4, a3, a3\n";
    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    EXPECT_EQ(result.stats.barrierLineCount, 1);
    int ecall = lineOf(result.reorderedText, "ecall");
    EXPECT_LT(lineOf(result.reorderedText, "add a2"), ecall);
    EXPECT_GT(lineOf(result.reorderedText, "lw a3"), ecall);
}

TEST(ReorderEngine, CommentsTravelWithTheirInstruction)
{
    const std::string program =
        "    lw a1, 0(a0)\n"
        "    add a2, a1, a1  # inline stays\n"
        "    # loads the second element\n"
        "    lw a3, 4(a0)\n"
        "    add a4, a3, a3\n";
    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    int comment = lineOf(result.reorderedText, "# loads the second element");
    ASSERT_GE(comment, 0);
    EXPECT_EQ(comment + 1, lineOf(result.reorderedText, "lw a3"));
    EXPECT_NE(result.reorderedText.find("add a2, a1, a1  # inline stays"), std::string::npos);
}

TEST(ReorderEngine, PseudoInstructionsAreNotSplit)
{
    // The li below expands to lui+addi; it must move as one line.
    const std::string program =
        "    lw a1, 0(a0)\n"
        "    add a2, a1, a1\n"
        "    li t0, 0x12345678\n"
        "    add a3, t0, t0\n";
    ReorderResult result = reorderProgram(program, PipelineMode::NoForwarding);
    EXPECT_NE(result.reorderedText.find("li t0, 0x12345678"), std::string::npos);
    EXPECT_LE(result.stats.stallsAfter, result.stats.stallsBefore);
    EXPECT_EQ(result.stats.instructionCount, 5);
}

TEST(ReorderEngine, DataSectionAndDirectivesAreUntouched)
{
    const std::string program =
        ".data\n"
        "arr: .word 1, 2, 3\n"
        "msg: .asciz \"a # b\"\n"
        ".text\n"
        ".globl main\n"
        "main:\n"
        "    lw a1, 0(a0)\n"
        "    add a2, a1, a1\n"
        "    lw a3, 4(a0)\n"
        "    add a4, a3, a3\n";
    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    std::vector<std::string> before = splitLinesForTest(program);
    std::vector<std::string> after = splitLinesForTest(result.reorderedText);
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < 6; ++i) // everything up to and including "main:"
    {
        EXPECT_EQ(before[i], after[i]);
    }
    EXPECT_EQ(result.stats.stallsAfter, 0);
}

TEST(ReorderEngine, LabelOnSameLineAsInstructionStaysAtBlockStart)
{
    const std::string program =
        "loop: lw a1, 0(a0)\n"
        "    add a2, a1, a1\n"
        "    lw a3, 4(a0)\n"
        "    add a4, a3, a3\n";
    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    EXPECT_EQ(lineOf(result.reorderedText, "loop:"), 0);
    EXPECT_EQ(result.stats.stallsAfter, 0);
}

TEST(ReorderEngine, PreservesLineEndingsAndTrailingNewline)
{
    const std::string crlf =
        "    lw a1, 0(a0)\r\n"
        "    add a2, a1, a1\r\n"
        "    lw a3, 4(a0)\r\n"
        "    add a4, a3, a3\r\n";
    ReorderResult result = reorderProgram(crlf, PipelineMode::Forwarding);
    EXPECT_EQ(result.stats.stallsAfter, 0);
    EXPECT_EQ(result.reorderedText.find("\n"), result.reorderedText.find("\r\n") + 1);
    EXPECT_EQ(result.reorderedText.back(), '\n');

    const std::string noTrailing =
        "    lw a1, 0(a0)\n    add a2, a1, a1\n    lw a3, 4(a0)\n    add a4, a3, a3";
    ReorderResult second = reorderProgram(noTrailing, PipelineMode::Forwarding);
    EXPECT_NE(second.reorderedText.back(), '\n');
}

TEST(ReorderEngine, EmptyAndTrivialInputs)
{
    EXPECT_EQ(reorderProgram("", PipelineMode::Forwarding).reorderedText, "");
    EXPECT_EQ(reorderProgram("", PipelineMode::Forwarding).stats.instructionCount, 0);
    EXPECT_EQ(reorderProgram("# nothing\n", PipelineMode::NoForwarding).reorderedText,
              "# nothing\n");
    EXPECT_EQ(reorderProgram("nop\n", PipelineMode::NoForwarding).stats.stallsAfter, 0);
}

TEST(ReorderEngine, SecondPassChangesNothing)
{
    const std::string program =
        "    lw a1, 0(a0)\n"
        "    add a2, a1, a1\n"
        "    lw a3, 4(a0)\n"
        "    add a4, a3, a3\n";
    ReorderResult first = reorderProgram(program, PipelineMode::Forwarding);
    ReorderResult second = reorderProgram(first.reorderedText, PipelineMode::Forwarding);
    EXPECT_EQ(second.reorderedText, first.reorderedText);
    EXPECT_EQ(second.stats.stallsBefore, first.stats.stallsAfter);
}

TEST(ReorderEngine, CarriesHistoryAcrossFallThroughBoundaries)
{
    // The first instruction of the second block must not be a consumer of the load that ends the
    // first block, because control falls straight through.
    const std::string program =
        "    lw a1, 0(a0)\n"
        "next:\n"
        "    add a2, a1, a1\n"
        "    addi a3, zero, 1\n";
    ReorderResult result = reorderProgram(program, PipelineMode::Forwarding);
    EXPECT_EQ(result.stats.stallsBefore, 1);
    EXPECT_EQ(result.stats.stallsAfter, 0);
    EXPECT_LT(lineOf(result.reorderedText, "addi a3"), lineOf(result.reorderedText, "add a2"));
}

TEST(ReorderEngine, LargeBlocksFallBackToGreedyAndNeverGetWorse)
{
    std::ostringstream program;
    for (int i = 0; i < 10; ++i)
    {
        program << "    lw t" << (i % 3) << ", " << (i * 4) << "(a0)\n";
        program << "    add a" << (i % 6) << ", t" << (i % 3) << ", t" << (i % 3) << "\n";
    }
    ReorderResult result = reorderProgram(program.str(), PipelineMode::Forwarding);
    EXPECT_TRUE(result.stats.usedGreedyFallback);
    EXPECT_LE(result.stats.stallsAfter, result.stats.stallsBefore);
}

TEST(ReorderEngine, ReorderedProgramIsMinimalForSmallBlocks)
{
    // Brute-force every permutation that respects dependencies and check the engine is optimal.
    const char *lines[] = {"lw a1, 0(a0)",   "add a2, a1, a1", "lw a3, 4(a0)",
                           "add a4, a3, a3", "lw a5, 8(a0)",   "add t0, a5, a2"};
    std::vector<Unit> units;
    for (const char *line : lines)
    {
        units.push_back(decodeInstruction(line));
    }
    for (size_t i = 0; i < units.size(); ++i)
    {
        units[i].sourceIndex = i;
    }

    std::vector<size_t> permutation = {0, 1, 2, 3, 4, 5};
    int best = 1 << 30;
    do
    {
        bool legal = true;
        for (size_t later = 0; later < permutation.size() && legal; ++later)
        {
            for (size_t earlier = 0; earlier < later; ++earlier)
            {
                // original order of the two units must be preserved if they conflict
                size_t a = permutation[earlier];
                size_t b = permutation[later];
                if (a > b && mustStayOrdered(units[b], units[a]))
                {
                    legal = false;
                    break;
                }
            }
        }
        if (!legal)
        {
            continue;
        }
        std::vector<MicroOp> stream;
        for (size_t index : permutation)
        {
            stream.insert(stream.end(), units[index].ops.begin(), units[index].ops.end());
        }
        best = std::min(best, countStalls(stream, PipelineMode::Forwarding));
    } while (std::next_permutation(permutation.begin(), permutation.end()));

    std::string program;
    for (const char *line : lines)
    {
        program += std::string("    ") + line + "\n";
    }
    EXPECT_EQ(reorderProgram(program, PipelineMode::Forwarding).stats.stallsAfter, best);
}

TEST(ReorderEngine, MalformedInputDoesNotCrashAndIsKeptVerbatim)
{
    const std::string program =
        "add\n"
        "add a0,\n"
        "lw a0, (\n"
        "lw a0, 4(\n"
        "lw a0, 4(zz)\n"
        "sw a0\n"
        "beq a0\n"
        "jalr\n"
        "li a0\n"
        "li a0, notanumber\n"
        "li a0, 99999999999999999999999\n"
        "fcvt.w a0, fa0\n"
        ":\n"
        "label:::\n"
        "\"unterminated\n"
        ".data\n"
        "add a0, a1, a2\n"
        ",,,,\n"
        "\t\t\n";
    for (PipelineMode mode : {PipelineMode::NoForwarding, PipelineMode::Forwarding})
    {
        ReorderResult result = reorderProgram(program, mode);
        EXPECT_EQ(result.reorderedText, program);
    }
}

// ---------------------------------------------------------------------------------------------
// Differential test: the reordered program must compute exactly the same thing.
// ---------------------------------------------------------------------------------------------

TEST(ReorderEngine, RandomProgramsKeepTheirBehaviour)
{
    std::mt19937 rng(12345);
    int improved = 0;
    for (int iteration = 0; iteration < 1500; ++iteration)
    {
        std::string program = randomProgram(rng, 4 + static_cast<int>(rng() % 20));

        Machine reference = freshMachine();
        run(reference, program);

        for (PipelineMode mode : {PipelineMode::NoForwarding, PipelineMode::Forwarding})
        {
            ReorderResult result = reorderProgram(program, mode);
            ASSERT_LE(result.stats.stallsAfter, result.stats.stallsBefore) << program;

            Machine candidate = freshMachine();
            run(candidate, result.reorderedText);
            ASSERT_TRUE(reference == candidate)
                << "behaviour changed (mode " << static_cast<int>(mode) << ")\n--- original\n"
                << program << "--- reordered\n"
                << result.reorderedText;

            // The reported 'after' number must agree with an independent count of the new text.
            ReorderResult recount = reorderProgram(result.reorderedText, mode);
            ASSERT_EQ(recount.stats.stallsBefore, result.stats.stallsAfter) << result.reorderedText;

            if (result.stats.stallsAfter < result.stats.stallsBefore)
            {
                ++improved;
            }
        }
    }
    EXPECT_GT(improved, 500); // the generator produces plenty of fixable code
}
