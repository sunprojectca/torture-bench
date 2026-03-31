/*
 * main.c — torture-bench entry point
 *
 * Usage:
 *   ./torture-bench [options]
 *
 * Options:
 *   -d <sec>      Duration per module (default: 10)
 *   -t <n>        Thread count (default: all cores)
 *   -s <hex>      Initial chain seed (default: random)
 *   -o <file>     Write JSON results to file (+ matching .txt report)
 *   --txt <file>  Write detailed text report to file
 *   -c <file>     Append CSV row to file
 *   --tune        Run anti-cheat tuning probe before benchmark
 *   --verbose     Extra output
 *   --list        List all modules and exit
 *   --only <name> Run only the named module
 *   --skip <name> Skip the named module
 *   --json        Print JSON to stdout after run
 */

#include "orchestrator.h"
#include "reporter.h"
#include "platform.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void derive_text_path(char *dst, size_t dst_size, const char *source_path)
{
    const char *dot;
    const char *slash;
    const char *slash_fwd;
    const char *slash_back;
    size_t prefix_len;

    if (!dst || dst_size == 0)
        return;
    dst[0] = 0;

    if (!source_path || strcmp(source_path, "-") == 0)
        return;

    slash_fwd = strrchr(source_path, '/');
    slash_back = strrchr(source_path, '\\');
    slash = slash_fwd;
    if (slash_back && (!slash || slash_back > slash))
        slash = slash_back;

    dot = strrchr(source_path, '.');
    prefix_len = strlen(source_path);
    if (dot && (!slash || dot > slash))
        prefix_len = (size_t)(dot - source_path);

    snprintf(dst, dst_size, "%.*s.txt", (int)prefix_len, source_path);
}

static void print_usage(void)
{
    printf(
        "Usage: torture-bench [options]\n\n"
        "  -d <sec>       Duration per module  (default: 10)\n"
        "  -t <n>         Thread count         (default: all cores)\n"
        "  -s <hex>       Initial seed (hex)   (default: time-based)\n"
        "  -o <file>      Write JSON to file (+ matching .txt report)\n"
        "  --txt <file>   Write detailed text report to file\n"
        "  -c <file>      Append CSV row to file\n"
        "  --tune         Run anti-cheat probe first\n"
        "  --verbose      Extra output\n"
        "  --list         List modules and exit\n"
        "  --only <name>  Run only this module\n"
        "  --skip <name>  Skip this module\n"
        "  --json         Print JSON to stdout\n\n");
}

static void print_banner(const platform_info_t *p)
{
    printf("\n");
    printf("  +===================================================+\n");
    printf("  |         TORTURE-BENCH  v1.0  CPU Fairness         |\n");
    printf("  +===================================================+\n\n");
    printf("  Platform:\n");
    platform_print(p);
    printf("\n");
}

int main(int argc, char **argv)
{
    bench_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.duration_sec = 10;
    cfg.thread_count = 0; /* 0 = auto */
    cfg.initial_seed = (uint64_t)time(NULL) ^ (uint64_t)(size_t)argv;
    cfg.verbose = 0;
    cfg.json_output = 0;
    cfg.tuning_mode = 0;
    cfg.output_file[0] = 0;

    char csv_file[512] = {0};
    char text_file[512] = {0};
    char only_mod[128] = {0};
    char skip_mods[MAX_MODULES][128];
    int n_skip = 0;
    int do_json_stdout = 0;

    /* ── parse args ────────────────────────────────────────────────────── */
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            cfg.duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            cfg.thread_count = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            cfg.initial_seed = (uint64_t)strtoull(argv[++i], NULL, 16);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            strncpy(cfg.output_file, argv[++i], sizeof(cfg.output_file) - 1);
        else if (strcmp(argv[i], "--txt") == 0 && i + 1 < argc)
            strncpy(text_file, argv[++i], sizeof(text_file) - 1);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            strncpy(csv_file, argv[++i], sizeof(csv_file) - 1);
        else if (strcmp(argv[i], "--tune") == 0)
            cfg.tuning_mode = 1;
        else if (strcmp(argv[i], "--verbose") == 0)
            cfg.verbose = 1;
        else if (strcmp(argv[i], "--json") == 0)
            do_json_stdout = 1;
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            strncpy(only_mod, argv[++i], sizeof(only_mod) - 1);
        else if (strcmp(argv[i], "--skip") == 0 && i + 1 < argc)
        {
            if (n_skip < MAX_MODULES)
                strncpy(skip_mods[n_skip++], argv[++i], 127);
        }
        else if (strcmp(argv[i], "--list") == 0)
        {
            /* print list */
            orchestrator_t o;
            bench_config_t tmp = cfg;
            orchestrator_init(&o, &tmp);
            orchestrator_register_all(&o);
            printf("Available modules:\n");
            for (int j = 0; j < o.count; j++)
                printf("  %-22s %s\n",
                       o.modules[j].name, o.modules[j].description);
            return 0;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage();
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    if (!text_file[0] && cfg.output_file[0])
        derive_text_path(text_file, sizeof(text_file), cfg.output_file);

    /* ── init ──────────────────────────────────────────────────────────── */
    orchestrator_t o;
    orchestrator_init(&o, &cfg);
    orchestrator_register_all(&o);

    /* Apply --only / --skip filters */
    if (only_mod[0])
    {
        for (int i = 0; i < o.count; i++)
            o.modules[i].enabled =
                (strcmp(o.modules[i].name, only_mod) == 0) ? 1 : 0;
    }
    for (int s = 0; s < n_skip; s++)
        for (int i = 0; i < o.count; i++)
            if (strcmp(o.modules[i].name, skip_mods[s]) == 0)
                o.modules[i].enabled = 0;

    print_banner(&o.platform);

    /* ── run ───────────────────────────────────────────────────────────── */
    int ret = orchestrator_run(&o);
    orchestrator_print_summary(&o);

    /* ── output ────────────────────────────────────────────────────────── */
    int output_failed = 0;

    if (cfg.output_file[0])
        if (reporter_write_json(&o, cfg.output_file) != 0)
        {
            fprintf(stderr, "Failed to write JSON report: %s\n", cfg.output_file);
            output_failed = 1;
        }

    if (text_file[0])
        if (reporter_write_text(&o, text_file) != 0)
        {
            fprintf(stderr, "Failed to write text report: %s\n", text_file);
            output_failed = 1;
        }
        else if (strcmp(text_file, "-") != 0)
        {
            printf("  Detailed text report: %s\n", text_file);
        }

    if (do_json_stdout)
        if (reporter_write_json(&o, "-") != 0)
        {
            fprintf(stderr, "Failed to write JSON to stdout\n");
            output_failed = 1;
        }

    if (csv_file[0])
        if (reporter_write_csv(&o, csv_file, 1 /* append */) != 0)
        {
            fprintf(stderr, "Failed to write CSV report: %s\n", csv_file);
            output_failed = 1;
        }


    return output_failed ? 1 : ret;
}
