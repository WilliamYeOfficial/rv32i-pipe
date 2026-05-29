#ifndef RV32I_PIPELINE_H
#define RV32I_PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IMEM_WORDS  1024
#define DMEM_BYTES  4096

typedef struct RV32CPU RV32CPU;

typedef struct {
    uint64_t cycles;
    uint64_t instrs;
    uint64_t stalls;
    uint64_t flushes;
    double   cpi;
} PipelineStats;

RV32CPU      *rv32_cpu_create(void);
void          rv32_cpu_destroy(RV32CPU *cpu);
void          rv32_cpu_reset(RV32CPU *cpu);

void          rv32_imem_write(RV32CPU *cpu, uint32_t word_idx, uint32_t instr);
uint32_t      rv32_dmem_read32(const RV32CPU *cpu, uint32_t byte_addr);

bool          rv32_step(RV32CPU *cpu);
void          rv32_run(RV32CPU *cpu, int max_cycles);

PipelineStats rv32_stats(const RV32CPU *cpu);
void          rv32_stats_print(const PipelineStats *s);

typedef struct {
    uint32_t words[IMEM_WORDS];
    size_t   count;
} RV32Program;

void rv32_program_append(RV32Program *p, uint32_t encoding);
void rv32_program_load(RV32CPU *cpu, const RV32Program *p);

#endif
