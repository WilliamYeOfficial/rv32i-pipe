# rv32i-pipeline

Cycle-accurate 5-stage RISC-V RV32I pipeline simulator with full data-hazard forwarding, load-use stall insertion, branch-not-taken speculation, and flush-on-mispredict. Reports cycles, instructions retired, stalls, flushes, and CPI.

---

## Pipeline Stages

```
Clock cycle →     1      2      3      4      5      6      7
                ┌─────┬──────┬──────┬──────┬──────┐
Instr 0 (ADD)   │ IF  │  ID  │  EX  │ MEM  │  WB  │
                └─────┼──────┼──────┼──────┼──────┼─────┐
Instr 1 (ADD)         │  IF  │  ID  │  EX  │ MEM  │  WB │
                      └──────┼──────┼──────┼──────┼─────┼──────┐
Instr 2 (LW)                 │  IF  │  ID  │  EX  │ MEM │  WB  │
                             └──────┴──────┴──────┴─────┴──────┘

IF  = Instruction Fetch     (PC + imem_read)
ID  = Instruction Decode    (register file read, control signals)
EX  = Execute               (ALU, branch resolution)
MEM = Memory Access         (load / store)
WB  = Write-Back            (register file write)
```

Pipeline latches: `IFIDReg`, `IDEXReg`, `EXMEMReg`, `MEMWBReg`. Each cycle all latches advance simultaneously.

---

## Instruction Encoding (RV32I)

```
 31      25 24   20 19  15 14  12 11      7 6      0
┌──────────┬───────┬──────┬──────┬──────────┬───────┐
│  funct7  │  rs2  │  rs1 │funct3│    rd    │opcode │  R-type
├──────────┴───────┼──────┼──────┼──────────┼───────┤
│     imm[11:0]    │  rs1 │funct3│    rd    │opcode │  I-type
├──────────┬───────┼──────┼──────┼──────────┼───────┤
│ imm[11:5]│  rs2  │  rs1 │funct3│ imm[4:0] │opcode │  S-type
├──────────┬───────┴──────┴──────┴──────────┼───────┤
│   imm    │         (rearranged bits)       │opcode │  B-type
└──────────┴────────────────────────────────┴───────┘

Supported opcodes: LUI, AUIPC, JAL, JALR, BRANCH, LOAD, STORE, OP-IMM, OP
```

---

## Data Hazard Forwarding

Two forwarding paths prevent stalls for back-to-back register dependencies:

```
         EX/MEM → ID/EX forwarding (1-cycle-old result)
         ┌─────────────────────────────────────────────┐
         │                                             │
  ADD x1,x2,x3    SUB x4,x1,x5    ← x1 forwarded from EX/MEM
         │                             ↑
         │         fwd_a = EX_alu_result (if EX/MEM.rd == ID/EX.rs1)
         └─────────────────────────────┘

         MEM/WB → ID/EX forwarding (2-cycle-old result)
         ┌───────────────────────────────────────────────────┐
         │                                                   │
  ADD x1,x2,x3    NOP    SUB x4,x1,x5    ← x1 from MEM/WB
         │                    ↑
         │    wb_val = WB.alu_result or WB.mem_data
         └────────────────────┘
                              (only if EX/MEM didn't already forward x1)
```

### Load-Use Hazard (1 bubble required)

A `LW` result is not available until the end of the MEM stage. If the immediately following instruction reads the loaded register, one stall cycle is inserted:

```
Cycle:       1      2      3      4      5      6      7
LW  x1,0(x2) IF    ID     EX     MEM ←─────────────────┐
ADD x3,x1,x4        IF    ID   [stall]   EX    MEM  WB  │
                           ↑                            │
                        ID stalls,                 x1 ready
                        IF stalls,                 after MEM
                        bubble injected
```

Detection: `ID/EX.mem_read && (ID/EX.rd == rs1 || ID/EX.rd == rs2)` of the IF/ID instruction.

---

## Control Hazard (Branch)

Branch outcomes are resolved at the end of the EX stage. The default policy is **branch-not-taken speculation**: the processor fetches the two instructions after the branch while EX executes.

```
Cycle:        1      2      3      4      5
BLT x1,x2,-8  IF    ID     EX ─── resolve
Instr A              IF    ID ←── flushed  (branch taken → wrong path)
Instr B                     IF ←── flushed
Target instr                       IF     ID     EX ...

On taken branch: flush IF/ID and ID/EX latches, redirect PC to branch target.
On not-taken:    no action needed, already fetching correct path.
```

---

## ALU Operations

```
ALU op   Encoding   Operation
───────  ────────   ─────────
ADD         0       a + b
SUB         1       a - b
SLL         2       a << (b & 31)
SLT         3       (int32)a < (int32)b ? 1 : 0
SLTU        4       a < b ? 1 : 0
XOR         5       a ^ b
SRL         6       a >> (b & 31)          (logical)
SRA         7       (int32)a >> (b & 31)   (arithmetic)
OR          8       a | b
AND         9       a & b
```

---

## Stage Execution Order Per Cycle

```
Each call to rv32_step(cpu) executes all 5 stages in reverse pipeline order
to avoid reading a latch that was written in the same cycle:

  1. WB   — write register file from MEM/WB latch
  2. MEM  — memory read/write, fill MEM/WB latch
  3. EX   — ALU, branch resolve, fill EX/MEM latch
             └─ if branch taken: flush IF/ID and ID/EX latches, redirect PC
  4. ID   — decode IF/ID, read registers, fill ID/EX latch
             └─ if load-use stall: hold IF/ID, inject bubble into ID/EX
  5. IF   — fetch new instruction into IF/ID latch (if not stalling)
             └─ stop fetching when pc ≥ prog_end_pc
```

---

## API

```c
// Lifecycle
RV32CPU *rv32_cpu_create(void);
void     rv32_cpu_destroy(RV32CPU *cpu);
void     rv32_cpu_reset(RV32CPU *cpu);

// Program loading
void rv32_program_append(RV32Program *p, uint32_t encoding);
void rv32_program_load(RV32CPU *cpu, const RV32Program *p);

// Execution
bool rv32_step(RV32CPU *cpu);               // single cycle
void rv32_run (RV32CPU *cpu, int max_cycles);

// Memory inspection
uint32_t rv32_dmem_read32(const RV32CPU *cpu, uint32_t byte_addr);

// Statistics
PipelineStats rv32_stats(const RV32CPU *cpu);
void          rv32_stats_print(const PipelineStats *s);
```

---

## Demo Program: sum(1..10)

```asm
addi x1, x0, 0      # i   = 0
addi x2, x0, 0      # sum = 0
addi x3, x0, 10     # limit = 10
loop:
addi x1, x1, 1      # i++
add  x2, x2, x1     # sum += i
blt  x1, x3, loop   # if i < 10 goto loop
sw   x2, 0(x0)      # store result to dmem[0]
ebreak
```

Expected result: `dmem[0] = 55`.

---

## Build

```sh
gcc -O2 -std=c11 rv32i_pipeline.c main.c -o rv32i-pipeline
```

---

## Sample Output

```
  sum(1..10) stored at dmem[0] = 55  (expected 55)

rv32i-pipeline: 5-stage RISC-V pipeline with forwarding

  cycles    : 61
  instrs    : 38
  stalls    : 0
  flushes   : 10
  CPI       : 1.61
```

CPI of 1.61 reflects the 10 branch flushes (2 wasted cycles each) from the loop back-edge. No load-use stalls occur in this program because no load result is consumed by the immediately following instruction.

---

## File Structure

```
rv32i-pipeline/
├── rv32i_pipeline.h    ← public API (opaque CPU handle, program builder, stats)
├── rv32i_pipeline.c    ← decode, ALU, forwarding, hazard detection, 5 stages
└── main.c              ← program encoding, run, result check
```
