#include "reporter.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static int result_count(const orchestrator_t *o)
{
        int count = 0;
        for (int i = 0; i < o->count; i++)
        {
                if (o->results[i].module_name)
                        count++;
        }
        return count;
}

static int warning_count(const orchestrator_t *o)
{
        int warnings = 0;
        for (int i = 0; i < o->count; i++)
        {
                if (o->results[i].module_name && o->results[i].coprocessor_suspected)
                        warnings++;
        }
        return warnings;
}

static double composite_score(const orchestrator_t *o)
{
        double composite = 0.0;
        int counted = 0;

        for (int i = 0; i < o->count; i++)
        {
                if (!o->results[i].module_name)
                        continue;
                composite += o->results[i].score;
                counted++;
        }

        return composite / (counted > 0 ? counted : 1);
}

static const char *verdict_for_warning_count(int warnings)
{
        return warnings == 0   ? "PURE_CPU_FAIR"
               : warnings <= 2 ? "MINOR_ACCELERATION_DETECTED"
                               : "SIGNIFICANT_ACCELERATION_DETECTED";
}

static const char *safe_text(const char *s)
{
        return (s && s[0]) ? s : "-";
}

static void append_list_item(char *buf, size_t buf_size, const char *item)
{
        size_t len;

        if (!buf || buf_size == 0 || !item || !item[0])
                return;

        len = strlen(buf);
        if (len >= buf_size - 1)
                return;

        snprintf(buf + len, buf_size - len, "%s%s", len ? ", " : "", item);
}

static void format_simd_summary(const platform_info_t *p, char *buf, size_t buf_size)
{
        if (!buf || buf_size == 0)
                return;

        buf[0] = '\0';
        if (p->has_neon)
                append_list_item(buf, buf_size, "NEON");
        if (p->has_avx2)
                append_list_item(buf, buf_size, "AVX2");
        if (p->has_avx512)
                append_list_item(buf, buf_size, "AVX512");
        if (p->has_sve)
                append_list_item(buf, buf_size, "SVE");

        if (!buf[0])
                snprintf(buf, buf_size, "None detected");
}

static void format_trait_summary(const platform_info_t *p, char *buf, size_t buf_size)
{
        if (!buf || buf_size == 0)
                return;

        buf[0] = '\0';
        if (p->is_apple_silicon)
                append_list_item(buf, buf_size, "Apple Silicon");
        if (p->is_snapdragon)
                append_list_item(buf, buf_size, "Snapdragon");
        if (p->is_wsl)
                append_list_item(buf, buf_size, "WSL");

        if (!buf[0])
                snprintf(buf, buf_size, "None");
}

static void json_escape(FILE *f, const char *s)
{
        fputc('"', f);
        for (; *s; s++)
        {
                if (*s == '"')
                        fputs("\\\"", f);
                else if (*s == '\\')
                        fputs("\\\\", f);
                else if (*s == '\n')
                        fputs("\\n", f);
                else
                        fputc(*s, f);
        }
        fputc('"', f);
}

int reporter_write_json(const orchestrator_t *o, const char *path)
{
        FILE *f = (strcmp(path, "-") == 0) ? stdout : fopen(path, "w");
        if (!f)
                return -1;

        const platform_info_t *p = &o->platform;
        double composite = composite_score(o);
        int warnings = warning_count(o);
        time_t now = time(NULL);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

        fprintf(f, "{\n");
        fprintf(f, "  \"torture_bench_version\": \"1.0.0\",\n");
        fprintf(f, "  \"timestamp\": \"%s\",\n", ts);
        fprintf(f, "  \"chain_proof_hash\": \"0x%016llx\",\n",
                (unsigned long long)o->final_chain_hash);
        fprintf(f, "  \"initial_seed\": \"0x%016llx\",\n",
                (unsigned long long)o->config.initial_seed);

        /* Platform */
        fprintf(f, "  \"platform\": {\n");
        fprintf(f, "    \"os\": \"%s\",\n", p->os);
        fprintf(f, "    \"arch\": \"%s\",\n", p->arch);
        fprintf(f, "    \"cpu\": ");
        json_escape(f, p->cpu_brand);
        fprintf(f, ",\n");
        fprintf(f, "    \"logical_cores\": %d,\n", p->logical_cores);
        fprintf(f, "    \"ram_gb\": %.1f,\n",
                (double)p->ram_bytes / (1024.0 * 1024.0 * 1024.0));
        fprintf(f, "    \"cache_l1_kb\": %d,\n", p->cache_l1_kb);
        fprintf(f, "    \"cache_l2_kb\": %d,\n", p->cache_l2_kb);
        fprintf(f, "    \"cache_l3_kb\": %d,\n", p->cache_l3_kb);
        fprintf(f, "    \"simd\": {");
        fprintf(f, "\"neon\":%s,\"avx2\":%s,\"avx512\":%s,\"sve\":%s",
                p->has_neon ? "true" : "false",
                p->has_avx2 ? "true" : "false",
                p->has_avx512 ? "true" : "false",
                p->has_sve ? "true" : "false");
        fprintf(f, "},\n");
        fprintf(f, "    \"is_apple_silicon\": %s,\n",
                p->is_apple_silicon ? "true" : "false");
        fprintf(f, "    \"is_snapdragon\": %s,\n",
                p->is_snapdragon ? "true" : "false");
        fprintf(f, "    \"is_wsl\": %s\n",
                p->is_wsl ? "true" : "false");
        fprintf(f, "  },\n");

        /* Config */
        fprintf(f, "  \"config\": {\n");
        fprintf(f, "    \"thread_count\": %d,\n", o->config.thread_count);
        fprintf(f, "    \"duration_sec\": %d,\n", o->config.duration_sec);
        fprintf(f, "    \"tuning_mode\": %s\n",
                o->config.tuning_mode ? "true" : "false");
        fprintf(f, "  },\n");

        /* Modules */
        fprintf(f, "  \"modules\": [\n");
        int written = 0;
        for (int i = 0; i < o->count; i++)
        {
                const bench_result_t *r = &o->results[i];
                if (!r->module_name)
                        continue;
                if (written > 0)
                        fprintf(f, ",\n");
                fprintf(f, "    {\n");
                fprintf(f, "      \"name\": \"%s\",\n", r->module_name);
                fprintf(f, "      \"score\": %.6f,\n", r->score);
                fprintf(f, "      \"ops_per_sec\": %.2f,\n", r->ops_per_sec);
                fprintf(f, "      \"wall_time_sec\": %.4f,\n", r->wall_time_sec);
                fprintf(f, "      \"chain_in\": \"0x%016llx\",\n",
                        (unsigned long long)r->chain_in);
                fprintf(f, "      \"chain_out\": \"0x%016llx\",\n",
                        (unsigned long long)r->chain_out);
                fprintf(f, "      \"coprocessor_suspected\": %s,\n",
                        r->coprocessor_suspected ? "true" : "false");
                fprintf(f, "      \"flags\": ");
                json_escape(f, r->flags);
                fprintf(f, ",\n");
                fprintf(f, "      \"notes\": ");
                json_escape(f, r->notes);
                fprintf(f, "\n");
                fprintf(f, "    }");
                written++;
        }
        if (written > 0)
                fprintf(f, "\n");
        fprintf(f, "  ],\n");

        /* Composite */
        fprintf(f, "  \"composite_score\": %.4f,\n", composite);
        fprintf(f, "  \"coprocessor_warnings\": %d,\n", warnings);
        fprintf(f, "  \"verdict\": \"%s\"\n", verdict_for_warning_count(warnings));
        fprintf(f, "}\n");

        if (f != stdout)
                fclose(f);
        return 0;
}

int reporter_write_text(const orchestrator_t *o, const char *path)
{
        FILE *f = (strcmp(path, "-") == 0) ? stdout : fopen(path, "w");
        if (!f)
                return -1;

        const platform_info_t *p = &o->platform;
        double composite = composite_score(o);
        int warnings = warning_count(o);
        int counted = result_count(o);
        int actual_threads = o->config.thread_count > 0 ? o->config.thread_count : p->logical_cores;
        time_t now = time(NULL);
        char ts[32];
        char simd[64];
        char traits[64];

        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        format_simd_summary(p, simd, sizeof(simd));
        format_trait_summary(p, traits, sizeof(traits));

        fprintf(f, "  +=====================================================================+\n");
        fprintf(f, "  |                 TORTURE-BENCH DETAILED TEXT REPORT                  |\n");
        fprintf(f, "  +=====================================================================+\n\n");

        fprintf(f, "  Generated        : %s\n", ts);
        fprintf(f, "  Verdict          : %s\n", verdict_for_warning_count(warnings));
        fprintf(f, "  Composite score  : %.4f\n", composite);
        fprintf(f, "  Modules executed : %d\n", counted);
        fprintf(f, "  Warnings         : %d\n", warnings);
        fprintf(f, "  Chain proof hash : 0x%016llx\n", (unsigned long long)o->final_chain_hash);
        fprintf(f, "  Initial seed     : 0x%016llx\n\n", (unsigned long long)o->config.initial_seed);

        fprintf(f, "  Platform\n");
        fprintf(f, "  -----------------------------------------------------------------------\n");
        fprintf(f, "  OS               : %s\n", p->os);
        fprintf(f, "  Arch             : %s\n", p->arch);
        fprintf(f, "  CPU              : %s\n", p->cpu_brand);
        fprintf(f, "  Logical cores    : %d\n", p->logical_cores);
        fprintf(f, "  RAM              : %.1f GB\n", (double)p->ram_bytes / (1024.0 * 1024.0 * 1024.0));
        fprintf(f, "  Cache (L1/L2/L3) : %d / %d / %d KB\n", p->cache_l1_kb, p->cache_l2_kb, p->cache_l3_kb);
        fprintf(f, "  SIMD             : %s\n", simd);
        fprintf(f, "  Traits           : %s\n\n", traits);

        fprintf(f, "  Run Configuration\n");
        fprintf(f, "  -----------------------------------------------------------------------\n");
        fprintf(f, "  Duration/module  : %d sec\n", o->config.duration_sec);
        fprintf(f, "  Thread count     : %d%s\n", actual_threads,
                o->config.thread_count > 0 ? "" : " (auto)");
        fprintf(f, "  Tuning mode      : %s\n", o->config.tuning_mode ? "enabled" : "disabled");
        fprintf(f, "  Verbose mode     : %s\n\n", o->config.verbose ? "enabled" : "disabled");

        if (warnings > 0)
        {
                fprintf(f, "  Warning Summary\n");
                fprintf(f, "  -----------------------------------------------------------------------\n");
                for (int i = 0; i < o->count; i++)
                {
                        const bench_result_t *r = &o->results[i];
                        if (!r->module_name || !r->coprocessor_suspected)
                                continue;

                        fprintf(f, "  - %-20s %s\n", r->module_name, safe_text(r->notes));
                }
                fprintf(f, "\n");
        }

        fprintf(f, "  Module Overview\n");
        fprintf(f, "  -----------------------------------------------------------------------\n");
        fprintf(f, "  #  %-22s %10s %13s %8s  %-16s\n", "Module", "Score", "Ops/sec", "Time", "Status");
        fprintf(f, "  -- ---------------------- ---------- ------------- --------  ----------------\n");
        int row = 0;
        for (int i = 0; i < o->count; i++)
        {
                const bench_result_t *r = &o->results[i];
                if (!r->module_name)
                        continue;

                row++;
                fprintf(f, "  %2d %-22s %10.2f %13.0f %8.2f  %-16s\n",
                        row,
                        r->module_name,
                        r->score,
                        r->ops_per_sec,
                        r->wall_time_sec,
                        r->coprocessor_suspected ? "COPROCESSOR" : "OK");
        }

        fprintf(f, "\n  Detailed Module Results\n");
        fprintf(f, "  -----------------------------------------------------------------------\n");
        row = 0;
        for (int i = 0; i < o->count; i++)
        {
                const bench_result_t *r = &o->results[i];
                if (!r->module_name)
                        continue;

                row++;
                fprintf(f, "  [%02d] %s - %s\n", row, r->module_name, safe_text(o->modules[i].description));
                fprintf(f, "       Score        : %.4f\n", r->score);
                fprintf(f, "       Ops/sec      : %.2f\n", r->ops_per_sec);
                fprintf(f, "       Wall time    : %.4f sec\n", r->wall_time_sec);
                fprintf(f, "       Chain        : 0x%016llx -> 0x%016llx\n",
                        (unsigned long long)r->chain_in,
                        (unsigned long long)r->chain_out);
                fprintf(f, "       Status       : %s\n",
                        r->coprocessor_suspected ? "Acceleration suspected" : "No coprocessor warning");
                fprintf(f, "       Flags        : %s\n", safe_text(r->flags));
                fprintf(f, "       Notes        : %s\n\n", safe_text(r->notes));
        }

        if (f != stdout)
                fclose(f);
        return 0;
}

int reporter_write_csv(const orchestrator_t *o, const char *path, int append)
{
        FILE *f = fopen(path, append ? "a" : "w");
        if (!f)
                return -1;

        const platform_info_t *p = &o->platform;

        /* Header if new file */
        if (!append)
        {
                fprintf(f, "cpu,arch,cores,");
                for (int i = 0; i < o->count; i++)
                {
                        if (o->results[i].module_name)
                                fprintf(f, "%s,", o->results[i].module_name);
                }
                fprintf(f, "composite,warnings,chain_hash\n");
        }

        /* Data row */
        fprintf(f, "\"%s\",%s,%d,", p->cpu_brand, p->arch, p->logical_cores);
        for (int i = 0; i < o->count; i++)
        {
                if (!o->results[i].module_name)
                        continue;
                fprintf(f, "%.4f,", o->results[i].score);
        }
        fprintf(f, "%.4f,%d,0x%016llx\n",
                composite_score(o), warning_count(o),
                (unsigned long long)o->final_chain_hash);

        fclose(f);
        return 0;
}
