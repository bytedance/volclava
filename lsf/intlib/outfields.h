/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#ifndef OUTPUT_FIELDS_H
#define OUTPUT_FIELDS_H

#include "outwire.h"
#include "fmtout.h"

/* Canonical names, aliases, rendering metadata, and wire dependencies share
 * one registry. Wire bits are stable within the typed output protocol and use
 * a 64-bit mask (1ULL << N) to selectively project fields across daemon RPCs.
 */

#define OUTPUT_JOB_FIELDS_COUNT 19
#define OUTPUT_HOST_FIELDS_COUNT 13
#define OUTPUT_QUEUE_FIELDS_COUNT 33
#define OUTPUT_LIM_FIELDS_COUNT 12

extern const struct fmt_field_def output_job_fields[OUTPUT_JOB_FIELDS_COUNT];
extern const struct fmt_field_def output_host_fields[OUTPUT_HOST_FIELDS_COUNT];
extern const struct fmt_field_def output_queue_fields[OUTPUT_QUEUE_FIELDS_COUNT];
extern const struct fmt_field_def output_lim_fields[OUTPUT_LIM_FIELDS_COUNT];

#define OUTPUT_FIELD_COUNT(table) ((int)(sizeof(table) / sizeof((table)[0])))
#define OUTPUT_MASK(fields, group) \
    fmt_fields_mask(fields, output_##group##_fields, OUTPUT_FIELD_COUNT(output_##group##_fields))

#endif /* OUTPUT_FIELDS_H */
