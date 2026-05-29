#include "rv32i_pipeline.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define OPCODE(x) ((x) & 0x7F)
#define RD(x)     (((x) >>  7) & 0x1F)
#define RS1(x)    (((x) >> 15) & 0x1F)
#define RS2(x)    (((x) >> 20) & 0x1F)
#define FUNCT3(x) (((x) >> 12) & 0x07)
#define FUNCT7(x) (((x) >> 25) & 0x7F)

#define OP_LUI    0x37
#define OP_AUIPC  0x17
#define OP_JAL    0x6F
#define OP_JALR   0x67
#define OP_BRANCH 0x63
#define OP_LOAD   0x03
#define OP_STORE  0x23
#define OP_IMM    0x13
#define OP_REG    0x33

typedef enum {
    ALU_ADD=0, ALU_SUB, ALU_SLL, ALU_SLT,
    ALU_SLTU,  ALU_XOR, ALU_SRL, ALU_SRA,
    ALU_OR,    ALU_AND
} AluOp;

typedef struct { uint32_t pc, instr; bool valid; } IFIDReg;

typedef struct {
    uint32_t pc, rs1_val, rs2_val, instr;
    int32_t  imm;
    uint8_t  rd, rs1, rs2, alu_op;
    bool     alu_src, mem_read, mem_write, reg_write;
    bool     branch, jal, jalr, valid;
} IDEXReg;

typedef struct {
    uint32_t pc, alu_result, rs2_val;
    uint8_t  rd;
    bool     mem_read, mem_write, reg_write;
    bool     branch_taken;
    uint32_t branch_target;
    bool     valid;
} EXMEMReg;

typedef struct {
    uint32_t alu_result, mem_data;
    uint8_t  rd;
    bool     reg_write, mem_to_reg, valid;
} MEMWBReg;

struct RV32CPU {
    uint32_t  reg[32];
    uint32_t  pc;
    uint32_t  prog_end_pc;
    uint32_t  imem[IMEM_WORDS];
    uint8_t   dmem[DMEM_BYTES];
    IFIDReg   if_id;
    IDEXReg   id_ex;
    EXMEMReg  ex_mem;
    MEMWBReg  mem_wb;
    uint64_t  cycles, instrs, stalls, flushes;
};

static int32_t imm_I(uint32_t i) { return (int32_t)i >> 20; }
static int32_t imm_S(uint32_t i) {
    return (int32_t)(((i >> 25) << 5) | ((i >> 7) & 0x1F)) << 20 >> 20;
}
static int32_t imm_B(uint32_t i) {
    return (int32_t)(((i >> 31) << 12) | (((i >> 7) & 1) << 11) |
                     (((i >> 25) & 0x3F) << 5) | (((i >> 8) & 0xF) << 1)) << 19 >> 19;
}
static int32_t imm_U(uint32_t i) { return (int32_t)(i & 0xFFFFF000); }
static int32_t imm_J(uint32_t i) {
    return (int32_t)(((i >> 31) << 20) | (((i >> 12) & 0xFF) << 12) |
                     (((i >> 20) & 1) << 11) | (((i >> 21) & 0x3FF) << 1)) << 11 >> 11;
}

static uint32_t alu_exec(AluOp op, uint32_t a, uint32_t b)
{
    switch (op) {
        case ALU_ADD:  return a + b;
        case ALU_SUB:  return a - b;
        case ALU_SLL:  return a << (b & 31);
        case ALU_SLT:  return (int32_t)a < (int32_t)b ? 1 : 0;
        case ALU_SLTU: return a < b ? 1 : 0;
        case ALU_XOR:  return a ^ b;
        case ALU_SRL:  return a >> (b & 31);
        case ALU_SRA:  return (uint32_t)((int32_t)a >> (b & 31));
        case ALU_OR:   return a | b;
        case ALU_AND:  return a & b;
    }
    return 0;
}

static void decode_instr(uint32_t instr, uint32_t pc, IDEXReg *e)
{
    memset(e, 0, sizeof *e);
    e->pc    = pc;
    e->instr = instr;
    e->valid = true;

    uint8_t op = OPCODE(instr);
    uint8_t f3 = FUNCT3(instr);
    uint8_t f7 = FUNCT7(instr);
    e->rd  = RD(instr);
    e->rs1 = RS1(instr);
    e->rs2 = RS2(instr);

    switch (op) {
        case OP_LUI:    e->imm=imm_U(instr); e->alu_src=1; e->reg_write=1;
                        e->rs1=0; e->alu_op=ALU_ADD; break;
        case OP_AUIPC:  e->imm=imm_U(instr); e->alu_src=1; e->reg_write=1;
                        e->alu_op=ALU_ADD; break;
        case OP_JAL:    e->imm=imm_J(instr); e->reg_write=1; e->jal=1; break;
        case OP_JALR:   e->imm=imm_I(instr); e->reg_write=1; e->jalr=1;
                        e->alu_src=1; e->alu_op=ALU_ADD; break;
        case OP_BRANCH: e->imm=imm_B(instr); e->branch=1;
                        e->alu_op=(f3==0||f3==1)?ALU_SUB:(f3==4||f3==5)?ALU_SLT:ALU_SLTU;
                        break;
        case OP_LOAD:   e->imm=imm_I(instr); e->mem_read=1; e->reg_write=1;
                        e->alu_src=1; e->alu_op=ALU_ADD; break;
        case OP_STORE:  e->imm=imm_S(instr); e->mem_write=1;
                        e->alu_src=1; e->alu_op=ALU_ADD; break;
        case OP_IMM:    e->imm=imm_I(instr); e->alu_src=1; e->reg_write=1;
                        if      (f3==0) e->alu_op=ALU_ADD;
                        else if (f3==2) e->alu_op=ALU_SLT;
                        else if (f3==3) e->alu_op=ALU_SLTU;
                        else if (f3==4) e->alu_op=ALU_XOR;
                        else if (f3==6) e->alu_op=ALU_OR;
                        else if (f3==7) e->alu_op=ALU_AND;
                        else if (f3==1) e->alu_op=ALU_SLL;
                        else            e->alu_op=(f7==0x20)?ALU_SRA:ALU_SRL;
                        break;
        case OP_REG:    e->reg_write=1;
                        if      (f3==0) e->alu_op=(f7==0x20)?ALU_SUB:ALU_ADD;
                        else if (f3==1) e->alu_op=ALU_SLL;
                        else if (f3==2) e->alu_op=ALU_SLT;
                        else if (f3==3) e->alu_op=ALU_SLTU;
                        else if (f3==4) e->alu_op=ALU_XOR;
                        else if (f3==5) e->alu_op=(f7==0x20)?ALU_SRA:ALU_SRL;
                        else if (f3==6) e->alu_op=ALU_OR;
                        else            e->alu_op=ALU_AND;
                        break;
        default: break;
    }
}

static uint32_t imem_fetch(const RV32CPU *cpu, uint32_t addr)
{
    uint32_t idx = addr / 4;
    return idx < IMEM_WORDS ? cpu->imem[idx] : 0u;
}

static uint32_t dmem_load(const RV32CPU *cpu, uint32_t addr)
{
    uint32_t v;
    memcpy(&v, cpu->dmem + addr, 4);
    return v;
}

static void dmem_store(RV32CPU *cpu, uint32_t addr, uint32_t v)
{
    memcpy(cpu->dmem + addr, &v, 4);
}

RV32CPU *rv32_cpu_create(void)
{
    RV32CPU *cpu = calloc(1, sizeof *cpu);
    return cpu;
}

void rv32_cpu_destroy(RV32CPU *cpu) { free(cpu); }

void rv32_cpu_reset(RV32CPU *cpu)
{
    memset(cpu, 0, sizeof *cpu);
}

void rv32_imem_write(RV32CPU *cpu, uint32_t word_idx, uint32_t instr)
{
    cpu->imem[word_idx] = instr;
}

uint32_t rv32_dmem_read32(const RV32CPU *cpu, uint32_t byte_addr)
{
    return dmem_load(cpu, byte_addr);
}

void rv32_program_append(RV32Program *p, uint32_t encoding)
{
    p->words[p->count++] = encoding;
}

void rv32_program_load(RV32CPU *cpu, const RV32Program *p)
{
    for (size_t i = 0; i < p->count; ++i)
        cpu->imem[i] = p->words[i];
    cpu->prog_end_pc = (uint32_t)(p->count * 4);
}

bool rv32_step(RV32CPU *cpu)
{
    cpu->cycles++;

    if (cpu->mem_wb.valid && cpu->mem_wb.reg_write && cpu->mem_wb.rd != 0)
        cpu->reg[cpu->mem_wb.rd] = cpu->mem_wb.mem_to_reg
            ? cpu->mem_wb.mem_data : cpu->mem_wb.alu_result;

    EXMEMReg *em = &cpu->ex_mem;
    MEMWBReg  wb = {0};
    if (em->valid) {
        wb.valid      = true;
        wb.alu_result = em->alu_result;
        wb.rd         = em->rd;
        wb.reg_write  = em->reg_write;
        wb.mem_to_reg = em->mem_read;
        if (em->mem_read)  wb.mem_data = dmem_load(cpu, em->alu_result);
        if (em->mem_write) dmem_store(cpu, em->alu_result, em->rs2_val);
        cpu->instrs++;
    }
    cpu->mem_wb = wb;

    IDEXReg  *de = &cpu->id_ex;
    EXMEMReg  ex = {0};

    if (de->valid) {
        ex.valid      = true;
        ex.rd         = de->rd;
        ex.reg_write  = de->reg_write;
        ex.mem_read   = de->mem_read;
        ex.mem_write  = de->mem_write;

        uint32_t fwd_a = de->rs1_val, fwd_b = de->rs2_val;

        if (em->valid && em->reg_write && em->rd != 0) {
            if (em->rd == de->rs1) fwd_a = em->alu_result;
            if (em->rd == de->rs2) fwd_b = em->alu_result;
        }
        if (cpu->mem_wb.valid && cpu->mem_wb.reg_write && cpu->mem_wb.rd != 0) {
            uint32_t wbv = cpu->mem_wb.mem_to_reg
                         ? cpu->mem_wb.mem_data : cpu->mem_wb.alu_result;
            if (cpu->mem_wb.rd == de->rs1 &&
                !(em->valid && em->reg_write && em->rd == de->rs1))
                fwd_a = wbv;
            if (cpu->mem_wb.rd == de->rs2 &&
                !(em->valid && em->reg_write && em->rd == de->rs2))
                fwd_b = wbv;
        }

        uint32_t alu_a = (OPCODE(de->instr) == OP_AUIPC || de->jal) ? de->pc : fwd_a;
        uint32_t alu_b = de->alu_src ? (uint32_t)de->imm : fwd_b;

        ex.alu_result = alu_exec((AluOp)de->alu_op, alu_a, alu_b);
        ex.rs2_val    = fwd_b;
        ex.pc         = de->pc;

        if (de->branch) {
            uint8_t f3 = FUNCT3(de->instr);
            bool t = false;
            switch (f3) {
                case 0: t = fwd_a == fwd_b; break;
                case 1: t = fwd_a != fwd_b; break;
                case 4: t = (int32_t)fwd_a <  (int32_t)fwd_b; break;
                case 5: t = (int32_t)fwd_a >= (int32_t)fwd_b; break;
                case 6: t = fwd_a <  fwd_b; break;
                case 7: t = fwd_a >= fwd_b; break;
            }
            ex.branch_taken  = t;
            ex.branch_target = de->pc + (uint32_t)de->imm;
        }
        if (de->jal) {
            ex.branch_taken  = true;
            ex.branch_target = de->pc + (uint32_t)de->imm;
            ex.alu_result    = de->pc + 4;
        }
        if (de->jalr) {
            ex.branch_taken  = true;
            ex.branch_target = ex.alu_result & ~1u;
            ex.alu_result    = de->pc + 4;
        }
    }

    bool flush = ex.valid && ex.branch_taken;
    if (flush) {
        cpu->pc    = ex.branch_target;
        cpu->if_id = (IFIDReg){0};
        cpu->id_ex = (IDEXReg){0};
        cpu->flushes++;
    }
    cpu->ex_mem = ex;

    bool stall = de->valid && de->mem_read &&
                 (de->rd == RS1(cpu->if_id.instr) ||
                  de->rd == RS2(cpu->if_id.instr));

    if (!flush) {
        IDEXReg id = {0};
        if (cpu->if_id.valid && !stall) {
            decode_instr(cpu->if_id.instr, cpu->if_id.pc, &id);
            id.rs1_val = cpu->reg[id.rs1];
            id.rs2_val = cpu->reg[id.rs2];
        }
        if (stall) { cpu->stalls++; cpu->cycles++; }
        else        cpu->id_ex = id;

        if (!stall) {
            if (cpu->pc < cpu->prog_end_pc) {
                cpu->if_id.pc    = cpu->pc;
                cpu->if_id.instr = imem_fetch(cpu, cpu->pc);
                cpu->if_id.valid = true;
                cpu->pc         += 4;
            } else {
                cpu->if_id = (IFIDReg){0};
            }
        }
    }

    return false;
}

void rv32_run(RV32CPU *cpu, int max_cycles)
{
    for (int c = 0; c < max_cycles; ++c) {
        bool pipeline_empty = !cpu->if_id.valid &&
                              !cpu->id_ex.valid &&
                              !cpu->ex_mem.valid;
        if (pipeline_empty && cpu->pc >= cpu->prog_end_pc) break;
        rv32_step(cpu);
    }
}

PipelineStats rv32_stats(const RV32CPU *cpu)
{
    return (PipelineStats){
        .cycles  = cpu->cycles,
        .instrs  = cpu->instrs,
        .stalls  = cpu->stalls,
        .flushes = cpu->flushes,
        .cpi     = cpu->instrs ? (double)cpu->cycles / (double)cpu->instrs : 0.0,
    };
}

void rv32_stats_print(const PipelineStats *s)
{
    printf("rv32i-pipeline: 5-stage RISC-V pipeline with forwarding\n\n");
    printf("  cycles    : %llu\n",  (unsigned long long)s->cycles);
    printf("  instrs    : %llu\n",  (unsigned long long)s->instrs);
    printf("  stalls    : %llu\n",  (unsigned long long)s->stalls);
    printf("  flushes   : %llu\n",  (unsigned long long)s->flushes);
    printf("  CPI       : %.2f\n",  s->cpi);
}
