# Code Reordering (Reorder tab)

The **Reorder** tab rearranges the instructions of the program in the main editor so the
5-stage pipeline stalls less, and shows the original and the reordered code side by side with
before/after stall counts.

## Using it

1. Write or open a program in the **Editor** tab.
2. Open the **Reorder** tab and choose the target pipeline with the switch:
   * **No forwarding** (switch off) - every instruction that uses a value produced by one of the
     previous two instructions waits for it to be written back.
   * **Forwarding** (switch on) - only a load followed directly by an instruction that uses the
     loaded value stalls.
   The switch follows the processor chosen under *Settings -> Processor* (and can be changed by hand).
3. Press **Reorder**. Moved lines are highlighted in the right-hand pane. Changing the switch
   afterwards re-runs the reordering for the other pipeline.
4. Press **Copy to main editor** to replace the editor contents with the reordered code.

## What it does and does not touch

| Kept as is | Reordered |
|---|---|
| Labels, directives, `.data` and every other non-code line | Instructions between two such lines |
| `ecall`, `ebreak`, `fence`, CSR instructions, and anything not recognised (for example custom pseudo-instructions) - nothing is moved across them | |
| Branches and jumps (always the last instruction of their block) | |

* Only independent instructions are swapped. Register dependencies (read-after-write,
  write-after-read, write-after-write) and memory dependencies are all preserved, so the program
  gives exactly the same results.
* Loads and stores are treated conservatively: a store never swaps with another load or store
  unless both use the same base register (unchanged in between) and touch different bytes.
* Pseudo-instructions that expand to several instructions (`li`, `la`, `call`, `tail`) move as one line.
* A comment or blank line directly above an instruction moves with it. A trailing `# comment`
  stays on its line. `label: instruction` on one line becomes two lines (label, then instruction).
* If reordering would not reduce the total stall count, the original text is returned unchanged.

## How stalls are counted

The counts mirror `check_data_hazard()` in `src/processor/rv5s/rv5s_hdu.cpp`:

| Pipeline | Rule |
|---|---|
| No forwarding | dependent instruction directly after the producer: 2 stalls; one instruction in between: 1 stall; two or more in between: none |
| Forwarding | load followed directly by a user of the loaded register: 1 stall; everything else: none |

Both integer and floating-point registers are tracked; `x0` never causes a hazard (`f0` does).
"Estimated cycles" is `instructions + 4 (pipeline fill) + stalls`.

Notes on accuracy:

* Counts are **static estimates for straight-line execution**; they do not depend on which branches a
  particular run takes, and control-hazard flushes are not included (reordering does not change them).
* The hardware model in `rv5s_hdu.cpp` compares the raw `rs1`/`rs2` bit-fields of the instruction word
  whatever the instruction format, so for instructions with immediates it can report a hazard the static
  model does not. Treat the numbers as a guide and confirm with the Processor tab.
* A block with up to 12 movable instructions is searched exhaustively (optimal for the model). Larger
  blocks use a greedy list scheduler and the tab says so.

## Code layout

| Path | Role |
|---|---|
| `src/reorder/reorder_types.h` | shared data types |
| `src/reorder/instruction_decoder.*` | line parsing, register read/write sets, `li` expansion length |
| `src/reorder/hazard_model.*` | the two stall models |
| `src/reorder/scheduler.*` | dependency graph, exact search and greedy fallback |
| `src/reorder/code_reorderer.*` | `reorderProgram()` - the public entry point |
| `src/ui/reorder_tab/` | the tab and the toggle-switch widget (built in code, no `.ui` file) |
| `test/reorder_test.cpp` | unit tests plus a randomized check that reordered programs compute the same results |

The engine has no Qt dependency, so it can be reused from the CLI later.

## Possible extensions

* Fill branch delay slots (the engine already ends each block at its terminator).
* Insert `nop`s for hazards that remain on the processors without hazard detection.
* Use the engine from `kites-cli` (`--reorder forwarding|nf`).
