#include "reorder/scheduler.h"

#include "reorder/hazard_model.h"

#include <algorithm>
#include <numeric>

namespace Kites
{
namespace reorder
{

namespace
{

/// Upper bound on search nodes so a pathological block cannot freeze the UI.
constexpr long kNodeBudget = 400000;

bool touchesMemory(const Unit &unit)
{
    return unit.isLoad || unit.isStore;
}

bool memoryMayAlias(const MemoryAccess &a, const MemoryAccess &b)
{
    if (!a.valid || !b.valid)
    {
        return true;
    }
    if (a.baseRegister != b.baseRegister || a.baseVersion != b.baseVersion)
    {
        return true;
    }
    bool disjoint = (a.offset + a.size <= b.offset) || (b.offset + b.size <= a.offset);
    return !disjoint;
}

/**
 * @brief Keeps track of the decode cycle of every micro-op placed so far, so stalls can be
 * computed incrementally while units are appended and removed during the search.
 */
class Timeline
{
  public:
    Timeline(PipelineMode mode, const std::vector<MicroOp> &history) : m_mode(mode)
    {
        for (const MicroOp &op : history)
        {
            push(op);
        }
        m_historyLength = m_ops.size();
    }

    /// Appends all micro-ops of a unit; returns the stalls it adds.
    int append(const Unit &unit)
    {
        int stalls = 0;
        for (const MicroOp &op : unit.ops)
        {
            stalls += push(op);
        }
        return stalls;
    }

    void remove(const Unit &unit)
    {
        for (size_t i = 0; i < unit.ops.size(); ++i)
        {
            m_ops.pop_back();
            m_cycles.pop_back();
        }
    }

    /// Stalls a unit would add if appended now (without keeping it).
    int probe(const Unit &unit)
    {
        int stalls = append(unit);
        remove(unit);
        return stalls;
    }

  private:
    int push(const MicroOp &op)
    {
        constexpr size_t kWindow = 3;
        size_t i = m_ops.size();
        int earliest = (i == 0) ? 0 : m_cycles[i - 1] + 1;
        int unstalled = earliest;
        size_t from = (i > kWindow) ? i - kWindow : 0;
        for (size_t j = from; j < i; ++j)
        {
            if (m_ops[j]->writes.intersects(op.reads))
            {
                earliest = std::max(earliest, m_cycles[j] + dependencyLatency(m_mode, *m_ops[j]));
            }
        }
        m_ops.push_back(&op);
        m_cycles.push_back(earliest);
        // History stalls are not counted; callers only look at what they append.
        return (i < m_historyLength) ? 0 : earliest - unstalled;
    }

    PipelineMode m_mode;
    std::vector<const MicroOp *> m_ops;
    std::vector<int> m_cycles;
    size_t m_historyLength = static_cast<size_t>(-1);
};

struct Graph
{
    std::vector<std::vector<size_t>> predecessors;
    std::vector<std::vector<size_t>> successors;
    std::vector<int> height; ///< longest latency-weighted path to the end of the segment
};

Graph buildGraph(const std::vector<Unit> &units, PipelineMode mode)
{
    const size_t count = units.size();
    Graph graph;
    graph.predecessors.resize(count);
    graph.successors.resize(count);
    graph.height.assign(count, 0);

    for (size_t later = 0; later < count; ++later)
    {
        for (size_t earlier = 0; earlier < later; ++earlier)
        {
            if (mustStayOrdered(units[earlier], units[later]))
            {
                graph.predecessors[later].push_back(earlier);
                graph.successors[earlier].push_back(later);
            }
        }
    }

    for (size_t i = count; i-- > 0;)
    {
        int best = 0;
        for (size_t successor : graph.successors[i])
        {
            int weight = 1;
            if (units[i].writes.intersects(units[successor].reads))
            {
                weight = dependencyLatency(mode, units[i].ops.back());
            }
            best = std::max(best, weight + graph.height[successor]);
        }
        graph.height[i] = best;
    }
    return graph;
}

/// Stalls of a complete order: history + units in `order` + terminator.
int evaluateOrder(const SegmentInput &input, const std::vector<size_t> &order, PipelineMode mode)
{
    Timeline timeline(mode, input.history);
    int stalls = 0;
    for (size_t index : order)
    {
        stalls += timeline.append(input.units[index]);
    }
    if (input.hasTerminator)
    {
        stalls += timeline.append(input.terminator);
    }
    return stalls;
}

std::vector<size_t> greedyOrder(const SegmentInput &input, const Graph &graph, PipelineMode mode)
{
    const size_t count = input.units.size();
    std::vector<size_t> order;
    std::vector<int> remainingPredecessors(count);
    std::vector<bool> placed(count, false);
    for (size_t i = 0; i < count; ++i)
    {
        remainingPredecessors[i] = static_cast<int>(graph.predecessors[i].size());
    }

    Timeline timeline(mode, input.history);
    for (size_t step = 0; step < count; ++step)
    {
        size_t chosen = count;
        int chosenStalls = 0;
        for (size_t i = 0; i < count; ++i)
        {
            if (placed[i] || remainingPredecessors[i] != 0)
            {
                continue;
            }
            int stalls = timeline.probe(input.units[i]);
            bool better = chosen == count || stalls < chosenStalls ||
                          (stalls == chosenStalls && graph.height[i] > graph.height[chosen]);
            if (better)
            {
                chosen = i;
                chosenStalls = stalls;
            }
        }
        timeline.append(input.units[chosen]);
        placed[chosen] = true;
        order.push_back(chosen);
        for (size_t successor : graph.successors[chosen])
        {
            --remainingPredecessors[successor];
        }
    }
    return order;
}

/// Branch-and-bound search for the order with the fewest stalls.
class ExactSearch
{
  public:
    ExactSearch(const SegmentInput &input, const Graph &graph, PipelineMode mode,
                std::vector<size_t> bestOrder, int bestStalls)
        : m_input(input), m_graph(graph), m_timeline(mode, input.history),
          m_bestOrder(std::move(bestOrder)), m_bestStalls(bestStalls),
          m_placed(input.units.size(), false), m_remaining(input.units.size())
    {
        for (size_t i = 0; i < input.units.size(); ++i)
        {
            m_remaining[i] = static_cast<int>(graph.predecessors[i].size());
        }
    }

    std::vector<size_t> run()
    {
        search(0);
        return m_bestOrder;
    }

  private:
    void search(int stallsSoFar)
    {
        if (++m_nodes > kNodeBudget)
        {
            return;
        }
        const size_t count = m_input.units.size();
        if (m_current.size() == count)
        {
            int total = stallsSoFar;
            if (m_input.hasTerminator)
            {
                total += m_timeline.probe(m_input.terminator);
            }
            if (total < m_bestStalls)
            {
                m_bestStalls = total;
                m_bestOrder = m_current;
            }
            return;
        }

        // Candidates, cheapest first so good orders are found early.
        std::vector<std::pair<int, size_t>> candidates;
        for (size_t i = 0; i < count; ++i)
        {
            if (!m_placed[i] && m_remaining[i] == 0)
            {
                candidates.emplace_back(m_timeline.probe(m_input.units[i]), i);
            }
        }
        std::sort(candidates.begin(), candidates.end());

        for (const auto &candidate : candidates)
        {
            int added = candidate.first;
            size_t index = candidate.second;
            if (stallsSoFar + added >= m_bestStalls)
            {
                continue; // cannot beat the best order found so far
            }
            m_timeline.append(m_input.units[index]);
            m_placed[index] = true;
            m_current.push_back(index);
            for (size_t successor : m_graph.successors[index])
            {
                --m_remaining[successor];
            }

            search(stallsSoFar + added);

            for (size_t successor : m_graph.successors[index])
            {
                ++m_remaining[successor];
            }
            m_current.pop_back();
            m_placed[index] = false;
            m_timeline.remove(m_input.units[index]);

            if (m_nodes > kNodeBudget)
            {
                return;
            }
        }
    }

    const SegmentInput &m_input;
    const Graph &m_graph;
    Timeline m_timeline;
    std::vector<size_t> m_bestOrder;
    int m_bestStalls;
    std::vector<bool> m_placed;
    std::vector<int> m_remaining;
    std::vector<size_t> m_current;
    long m_nodes = 0;
};

} // namespace

bool mustStayOrdered(const Unit &earlier, const Unit &later)
{
    if (earlier.isBarrier || later.isBarrier)
    {
        return true;
    }
    if (earlier.writes.intersects(later.reads))
    {
        return true; // read after write
    }
    if (earlier.reads.intersects(later.writes))
    {
        return true; // write after read
    }
    if (earlier.writes.intersects(later.writes))
    {
        return true; // write after write
    }
    if (touchesMemory(earlier) && touchesMemory(later) && (earlier.isStore || later.isStore))
    {
        return memoryMayAlias(earlier.memory, later.memory);
    }
    return false;
}

ScheduleResult scheduleSegment(const SegmentInput &input, PipelineMode mode, size_t exactLimit)
{
    ScheduleResult result;
    const size_t count = input.units.size();

    std::vector<size_t> original(count);
    std::iota(original.begin(), original.end(), size_t{0});

    result.stallsBefore = evaluateOrder(input, original, mode);
    result.order = original;
    result.stallsAfter = result.stallsBefore;

    if (count < 2 || result.stallsBefore == 0)
    {
        return result; // nothing to gain
    }

    Graph graph = buildGraph(input.units, mode);

    // Start from the better of "keep as is" and the greedy schedule.
    std::vector<size_t> bestOrder = original;
    int bestStalls = result.stallsBefore;
    std::vector<size_t> greedy = greedyOrder(input, graph, mode);
    int greedyStalls = evaluateOrder(input, greedy, mode);
    if (greedyStalls < bestStalls)
    {
        bestOrder = greedy;
        bestStalls = greedyStalls;
    }

    if (count <= exactLimit)
    {
        ExactSearch search(input, graph, mode, bestOrder, bestStalls);
        bestOrder = search.run();
        bestStalls = evaluateOrder(input, bestOrder, mode);
    }
    else
    {
        result.usedGreedy = true;
    }

    result.order = bestOrder;
    result.stallsAfter = bestStalls;
    return result;
}

} // namespace reorder
} // namespace Kites
