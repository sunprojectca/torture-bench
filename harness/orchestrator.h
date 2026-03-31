#pragma once
#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "common.h"
#include "platform.h"

#define MAX_MODULES 32

typedef struct {
    bench_module_t  modules[MAX_MODULES];
    int             count;
    bench_config_t  config;
    platform_info_t platform;
    bench_result_t  results[MAX_MODULES];
    uint64_t        final_chain_hash;
} orchestrator_t;

void orchestrator_init(orchestrator_t *o, const bench_config_t *cfg);
void orchestrator_register_all(orchestrator_t *o);
int  orchestrator_run(orchestrator_t *o);
void orchestrator_print_summary(const orchestrator_t *o);

#endif
