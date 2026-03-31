#pragma once
#ifndef REPORTER_H
#define REPORTER_H

#include "orchestrator.h"

/* Write full JSON result to file (or stdout if path is "-") */
int reporter_write_json(const orchestrator_t *o, const char *path);

/* Write a detailed human-readable text report to file (or stdout if path is "-") */
int reporter_write_text(const orchestrator_t *o, const char *path);

/* Write a compact CSV line (for multi-machine comparison) */
int reporter_write_csv(const orchestrator_t *o, const char *path, int append);

#endif
