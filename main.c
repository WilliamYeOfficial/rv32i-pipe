#include "rv32i_pipeline.h"
#include <stdio.h>

int main(void)
{
    RV32Program prog = {0};
    rv32_program_append(&prog, 0x00000093);
    rv32_program_append(&prog, 0x00000113);
    rv32_program_append(&prog, 0x00A00193);
    rv32_program_append(&prog, 0x00108093);
    rv32_program_append(&prog, 0x00110133);
    rv32_program_append(&prog, 0xFE30CCE3);
    rv32_program_append(&prog, 0x00202023);
    rv32_program_append(&prog, 0x00100073);

    RV32CPU *cpu = rv32_cpu_create();
    rv32_program_load(cpu, &prog);
    rv32_run(cpu, 10000);

    uint32_t result = rv32_dmem_read32(cpu, 0);
    printf("  sum(1..10) stored at dmem[0] = %u  (expected 55)\n\n", result);

    PipelineStats s = rv32_stats(cpu);
    rv32_stats_print(&s);

    rv32_cpu_destroy(cpu);
    return 0;
}
