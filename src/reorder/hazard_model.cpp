#include "reorder/hazard_model.h"

#include <algorithm>

namespace Kites
{
namespace reorder
{

int dependencyLatency(PipelineMode mode, const MicroOp &producer)
{
    if (mode == PipelineMode::NoForwarding)
    {
        return 3;
    }
    return producer.isLoad ? 2 : 1;
}

int countStalls(const std::vector<const MicroOp *> &ops, size_t firstCounted, PipelineMode mode)
{
    // The largest latency is 3 decode cycles, and every instruction takes at least one cycle,
    // so only the three instructions before the current one can still matter.
    constexpr size_t kWindow = 3;

    std::vector<int> decodeCycle(ops.size(), 0);
    int stalls = 0;

    for (size_t i = 0; i < ops.size(); ++i)
    {
        int earliest = (i == 0) ? 0 : decodeCycle[i - 1] + 1;
        int unstalled = earliest;

        size_t from = (i > kWindow) ? i - kWindow : 0;
        for (size_t j = from; j < i; ++j)
        {
            if (ops[j]->writes.intersects(ops[i]->reads))
            {
                earliest = std::max(earliest, decodeCycle[j] + dependencyLatency(mode, *ops[j]));
            }
        }

        decodeCycle[i] = earliest;
        if (i >= firstCounted)
        {
            stalls += earliest - unstalled;
        }
    }
    return stalls;
}

int countStalls(const std::vector<MicroOp> &ops, PipelineMode mode)
{
    std::vector<const MicroOp *> pointers;
    pointers.reserve(ops.size());
    for (const MicroOp &op : ops)
    {
        pointers.push_back(&op);
    }
    return countStalls(pointers, 0, mode);
}

} // namespace reorder
} // namespace Kites
