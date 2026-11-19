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

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#ifndef FMT_OUTPUT_H
#define FMT_OUTPUT_H

#include <stddef.h>
#include <stdio.h>
#include "cJSON.h"

/*
 * Field definition flags.
 * FMT_FIELD_KEEP_TAIL: Truncate from the left and prefix with '*' to preserve
 * the trailing part of values (e.g. execution host or job name).
 */
#define FMT_FIELD_KEEP_TAIL 1u

/* Metadata for a supported custom output field. */
struct fmt_field_def {
    const char *name;            /* Primary canonical field name (e.g. "JOBID") */
    const char *alias;           /* Alternative alias name (e.g. "ID"), or NULL */
    const char *header;          /* Display header string for tabular text output */
    int default_width;           /* Default column width in characters */
    unsigned int flags;          /* Formatting flags (e.g. FMT_FIELD_KEEP_TAIL) */
    const char *json_key;        /* Key name for JSON output; NULL defaults to name */
    unsigned long long wire_mask;/* Bitmask for selective daemon RPC requests */
};

/* Parsed output column specification. */
struct fmt_column {
    const struct fmt_field_def *field; /* Pointer to field metadata */
    int width;                         /* Target column width in characters */
    int right_align;                   /* 1 if right-aligned, 0 for left-aligned */
    int has_width;                     /* 1 if width was explicitly specified */
};

/* Parsed custom output request containing all columns and layout options. */
struct fmt_request {
    struct fmt_column *columns;        /* Dynamically allocated column array */
    int num_columns;                   /* Number of requested columns */
    char delimiter;                    /* Field delimiter character */
    int has_delimiter;                 /* 1 if custom delimiter was specified */
};

/*
 * Parse a custom output format string (e.g. "jobid:10 stat:- delimiter='|'").
 * Populates 'request' with parsed columns and delimiter settings.
 * Returns 0 on success, or -1 on failure with error details written to 'errbuf'.
 * On success, the caller must call fmt_output_free() when done with 'request'.
 */
int fmt_output_parse(const char *format, const struct fmt_field_def *fields,
                     int num_fields, struct fmt_request *request,
                     char *errbuf, size_t errbuf_len);

/*
 * Free internal memory allocated for 'request' and zero out its members.
 */
void fmt_output_free(struct fmt_request *request);

/*
 * Serialize all requested canonical field names into 'buf' separated by spaces.
 * Returns the total buffer length needed (including terminating NUL), similar
 * to snprintf, or -1 on error.
 */
int fmt_output_fields_string(const struct fmt_request *request,
                             char *buf, size_t buflen);

/*
 * Allocate and return a space-separated string of requested canonical field names.
 * Returns a dynamically allocated string (caller must free), or NULL on error.
 */
char *fmt_output_fields_dup(const struct fmt_request *request);

/*
 * Check whether 'field_name' is included in the parsed request's columns.
 * Returns 1 if requested, 0 otherwise.
 */
int fmt_output_field_requested(const struct fmt_request *request,
                               const char *field_name);

/*
 * Print tabular header row to 'out' according to column widths and delimiter.
 */
void fmt_output_print_header(FILE *out, const struct fmt_request *request);

/*
 * Print a single column value to 'out', applying width, alignment and delimiters.
 */
void fmt_output_print_value(FILE *out, const struct fmt_request *request,
                            int column_index, const char *value);

/*
 * Print end-of-line newline character to 'out'.
 */
void fmt_output_print_eol(FILE *out);

/*
 * Compute the combined wire_mask for all field names appearing in 'fields'.
 */
unsigned long long fmt_fields_mask(const char *fields,
                                   const struct fmt_field_def *defs, int count);

/*
 * Check if field 'name' is present in the space-separated string 'fields'.
 * Returns 1 if present, 0 otherwise.
 */
int fmt_fields_requested(const char *fields, const char *name);

/*
 * Append a field value to a cJSON record object using the field's json_key.
 * Values are unpadded strings; duplicate fields are emitted only once.
 * Returns 0 on success, or -1 on failure.
 */
int fmt_json_value(cJSON *record, const struct fmt_request *request,
                   int column_index, const char *value);

/*
 * Render the final JSON object containing COMMAND, count and RECORDS array to 'out'.
 * Note: takes ownership of 'records' and frees it even if an error occurs.
 * Returns 0 on success, or -1 on failure.
 */
int fmt_json_print(FILE *out, const char *command, const char *count_key,
                   cJSON *records);

#endif
