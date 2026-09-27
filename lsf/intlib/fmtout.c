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

#include "fmtout.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int
fmt_set_error(char *errbuf, size_t errbuf_len, const char *msg,
              const char *arg)
{
    if (errbuf && errbuf_len > 0) {
        if (arg)
            snprintf(errbuf, errbuf_len, msg, arg);
        else
            snprintf(errbuf, errbuf_len, "%s", msg);
    }

    return -1;
}

static int
fmt_name_equal(const char *name, const char *token, size_t token_len)
{
    size_t i;

    if (!name)
        return 0;

    for (i = 0; i < token_len; i++) {
        if (name[i] == '\0')
            return 0;
        if (toupper((unsigned char)name[i]) !=
            toupper((unsigned char)token[i]))
            return 0;
    }

    return name[token_len] == '\0';
}

static const struct fmt_field_def *
fmt_find_field(const struct fmt_field_def *fields, int num_fields,
               const char *token, size_t token_len)
{
    int i;

    for (i = 0; i < num_fields; i++) {
        if (fmt_name_equal(fields[i].name, token, token_len) ||
            fmt_name_equal(fields[i].alias, token, token_len))
            return &fields[i];
    }

    return NULL;
}

static int
fmt_parse_width(const char *text, int default_width, int *width,
                int *right_align)
{
    long value;
    char *end = NULL;

    *right_align = 0;

    if (*text == '-') {
        *right_align = 1;
        text++;
    }

    if (*text == '\0') {
        *width = default_width;
        return 0;
    }

    value = strtol(text, &end, 10);
    if (*end != '\0' || value <= 0 || value > 4096)
        return -1;

    *width = (int)value;
    return 0;
}

static int
fmt_parse_delimiter(const char *token, char *delimiter)
{
    const char prefix[] = "delimiter=";
    const char *value;
    size_t len;
    size_t i;
    size_t prefix_len = strlen(prefix);

    if (strlen(token) < prefix_len)
        return 0;

    for (i = 0; prefix[i] != '\0'; i++) {
        if (toupper((unsigned char)token[i]) !=
            toupper((unsigned char)prefix[i]))
            return 0;
    }

    value = token + strlen(prefix);
    len = strlen(value);
    if ((len == 3 && value[0] == '\'' && value[2] == '\'') ||
        (len == 3 && value[0] == '"' && value[2] == '"')) {
        *delimiter = value[1];
        return 1;
    }

    if (len == 1) {
        *delimiter = value[0];
        return 1;
    }

    return -1;
}

static const char *
fmt_token_end(const char *p)
{
    char quote = '\0';

    while (*p) {
        if (quote != '\0') {
            if (*p == quote)
                quote = '\0';
            p++;
            continue;
        }

        if (*p == '\'' || *p == '"') {
            quote = *p;
            p++;
            continue;
        }

        if (isspace((unsigned char)*p))
            break;

        p++;
    }

    return p;
}

static void
fmt_print_field(FILE *out, const char *value, int width, int right_align,
                int keep_tail)
{
    size_t len;
    size_t output_len;
    int padding;

    if (!value)
        value = "-";

    len = strlen(value);
    if (width <= 0) {
        fputs(value, out);
        return;
    }

    output_len = len > (size_t)width ? (size_t)width : len;
    if (len > output_len && keep_tail) {
        size_t start = len - output_len + 1;
        /* Advance past continuation bytes before retaining a UTF-8 tail. */
        while (start < len && ((unsigned char)value[start] & 0xc0) == 0x80)
            start++;
        output_len = len - start + 1;
        padding = width - (int)output_len;
        if (right_align)
            while (padding-- > 0) fputc(' ', out);
        fputc('*', out);
        fwrite(value + start, 1, len - start, out);
    } else {
        if (output_len < len)
            while (output_len > 0 &&
                   ((unsigned char)value[output_len] & 0xc0) == 0x80)
                output_len--;
        padding = width - (int)output_len;
        if (right_align)
            while (padding-- > 0) fputc(' ', out);
        fwrite(value, 1, output_len, out);
    }
    if (!right_align)
        while (padding-- > 0) fputc(' ', out);
}

int
fmt_output_parse(const char *format, const struct fmt_field_def *fields,
                 int num_fields, struct fmt_request *request,
                 char *errbuf, size_t errbuf_len)
{
    const char *p;
    int capacity = 8;

    if (!request)
        return -1;

    memset(request, 0, sizeof(*request));

    if (!fields || num_fields <= 0 || !format || *format == '\0')
        return fmt_set_error(errbuf, errbuf_len,
                             "Empty custom output format.", NULL);

    request->columns = calloc(capacity, sizeof(struct fmt_column));
    if (!request->columns)
        return fmt_set_error(errbuf, errbuf_len, "Out of memory.", NULL);

    p = format;
    while (*p) {
        const char *token_start;
        size_t token_len;
        char *token;
        char delimiter;
        char *width_pos;
        const char *field_start;
        size_t field_len;
        int width = 0;
        int right_align = 0;
        int has_width = 0;
        const struct fmt_field_def *field;

        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            break;

        token_start = p;
        p = fmt_token_end(p);
        token_len = (size_t)(p - token_start);

        token = malloc(token_len + 1);
        if (!token) {
            fmt_output_free(request);
            return fmt_set_error(errbuf, errbuf_len, "Out of memory.", NULL);
        }
        memcpy(token, token_start, token_len);
        token[token_len] = '\0';

        delimiter = '\0';
        switch (fmt_parse_delimiter(token, &delimiter)) {
        case 1:
            if (request->has_delimiter) {
                free(token);
                fmt_output_free(request);
                return fmt_set_error(errbuf, errbuf_len,
                                     "Duplicate delimiter option.", NULL);
            }
            request->delimiter = delimiter;
            request->has_delimiter = 1;
            free(token);
            continue;
        case -1:
        {
            int rc;

            rc = fmt_set_error(errbuf, errbuf_len,
                               "Invalid delimiter <%s>. Use one character.",
                               token);
            free(token);
            fmt_output_free(request);
            return rc;
        }
        default:
            break;
        }

        field_start = token;
        field_len = strlen(token);
        width_pos = strchr(token, ':');
        if (!width_pos)
            width_pos = strchr(token, '%');
        if (width_pos) {
            field_len = (size_t)(width_pos - token);
            has_width = 1;
        }

        if (field_len == 0) {
            int rc;

            rc = fmt_set_error(errbuf, errbuf_len,
                               "Invalid empty field in <%s>.", token);
            free(token);
            fmt_output_free(request);
            return rc;
        }

        field = fmt_find_field(fields, num_fields, field_start, field_len);
        if (!field) {
            int rc;

            rc = fmt_set_error(errbuf, errbuf_len,
                               "<%s> in the format string is not a valid field name.",
                               token);
            free(token);
            fmt_output_free(request);
            return rc;
        }

        if (has_width &&
            fmt_parse_width(width_pos + 1, field->default_width,
                            &width, &right_align) < 0) {
            int rc;

            rc = fmt_set_error(errbuf, errbuf_len,
                               "Invalid output width in <%s>.", token);
            free(token);
            fmt_output_free(request);
            return rc;
        }

        if (request->num_columns == capacity) {
            struct fmt_column *columns;

            capacity *= 2;
            columns = realloc(request->columns,
                              capacity * sizeof(struct fmt_column));
            if (!columns) {
                free(token);
                fmt_output_free(request);
                return fmt_set_error(errbuf, errbuf_len, "Out of memory.",
                                     NULL);
            }
            request->columns = columns;
        }

        request->columns[request->num_columns].field = field;
        request->columns[request->num_columns].width = width;
        request->columns[request->num_columns].right_align = right_align;
        request->columns[request->num_columns].has_width = has_width;
        request->num_columns++;
        free(token);
    }

    if (request->num_columns == 0) {
        fmt_output_free(request);
        return fmt_set_error(errbuf, errbuf_len,
                             "No output fields were requested.", NULL);
    }

    return 0;
}

void
fmt_output_free(struct fmt_request *request)
{
    if (!request)
        return;

    free(request->columns);
    memset(request, 0, sizeof(*request));
}

int
fmt_output_fields_string(const struct fmt_request *request,
                         char *buf, size_t buflen)
{
    size_t needed = 1;
    size_t used = 0;
    int i;

    if (buf && buflen > 0)
        buf[0] = '\0';

    if (!request || request->num_columns < 0 ||
        (request->num_columns && !request->columns))
        return -1;

    for (i = 0; i < request->num_columns; i++) {
        const char *name;
        size_t len;
        if (!request->columns[i].field || !request->columns[i].field->name)
            return -1;
        name = request->columns[i].field->name;
        len = strlen(name);

        if (len > INT_MAX - needed - 1)
            return -1;
        needed += len;
        if (i > 0)
            needed++;

        if (buf && buflen > 0) {
            if (i > 0 && used + 1 < buflen)
                buf[used++] = ' ';
            if (used < buflen) {
                size_t copy_len = len;

                if (copy_len > buflen - used - 1)
                    copy_len = buflen - used - 1;
                memcpy(buf + used, name, copy_len);
                used += copy_len;
                buf[used] = '\0';
            }
        }
    }

    return (int)needed;
}

char *
fmt_output_fields_dup(const struct fmt_request *request)
{
    char *fields;
    int needed;

    needed = fmt_output_fields_string(request, NULL, 0);
    if (needed <= 0)
        return NULL;

    fields = malloc((size_t)needed);
    if (!fields)
        return NULL;
    if (fmt_output_fields_string(request, fields, (size_t)needed) != needed) {
        free(fields);
        return NULL;
    }

    return fields;
}

int
fmt_output_field_requested(const struct fmt_request *request,
                           const char *field_name)
{
    int i;

    if (!request || !request->columns || !field_name)
        return 0;

    for (i = 0; i < request->num_columns; i++) {
        if (request->columns[i].field && request->columns[i].field->name &&
            strcmp(request->columns[i].field->name, field_name) == 0)
            return 1;
    }

    return 0;
}

void
fmt_output_print_header(FILE *out, const struct fmt_request *request)
{
    int i;

    if (!out || !request || !request->columns)
        return;
    for (i = 0; i < request->num_columns; i++) {
        const struct fmt_column *column = &request->columns[i];

        if (!column->field) return;
        if (i > 0)
            fputc(request->has_delimiter ? request->delimiter : ' ', out);
        fmt_print_field(out, column->field->header, column->width,
                        column->right_align, 0);
    }
    fmt_output_print_eol(out);
}

void
fmt_output_print_value(FILE *out, const struct fmt_request *request,
                       int column_index, const char *value)
{
    const struct fmt_column *column;

    if (!out || !request || !request->columns || column_index < 0 ||
        column_index >= request->num_columns ||
        !request->columns[column_index].field)
        return;

    if (column_index > 0) {
        if (request->has_delimiter)
            fputc(request->delimiter, out);
        else
            fputc(' ', out);
    }

    column = &request->columns[column_index];
    fmt_print_field(out, value, column->width, column->right_align,
                    (column->field->flags & FMT_FIELD_KEEP_TAIL) != 0);
}

void
fmt_output_print_eol(FILE *out)
{
    if (out) fputc('\n', out);
}

int
fmt_fields_requested(const char *fields, const char *name)
{
    const char *p;
    size_t nameLen;

    if (!fields || fields[0] == '\0' || !name)
        return 0;

    nameLen = strlen(name);
    p = fields;
    while (*p) {
        const char *start;
        size_t len;

        while (*p && isspace((unsigned char)*p))
            p++;
        start = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        len = (size_t)(p - start);
        if (len == nameLen && strncasecmp(start, name, nameLen) == 0)
            return 1;
    }

    return 0;
}

int
fmt_json_value(cJSON *record, const struct fmt_request *request,
               int index, const char *value)
{
    const struct fmt_field_def *field;
    const char *key;
    cJSON *item;
    if (!record || !request || !request->columns || index < 0 ||
        index >= request->num_columns || !request->columns[index].field)
        return -1;
    field = request->columns[index].field;
    key = field->json_key ? field->json_key : field->name;
    if (cJSON_GetObjectItem(record, key))
        return 0;
    item = cJSON_CreateString(value ? value : "-");
    if (!item)
        return -1;
    if (!cJSON_AddItemToObject(record, key, item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

/* Takes ownership of records, including on failure. */
int
fmt_json_print(FILE *out, const char *command, const char *count_key,
               cJSON *records)
{
    cJSON *root = cJSON_CreateObject();
    char *text;
    int rc;
    if (!root || !records || !out || !command || !count_key) {
        cJSON_Delete(root);
        cJSON_Delete(records);
        return -1;
    }
    if (!cJSON_AddStringToObject(root, "COMMAND", command) ||
        !cJSON_AddNumberToObject(root, count_key, cJSON_GetArraySize(records)) ||
        !cJSON_AddItemToObject(root, "RECORDS", records)) {
        cJSON_Delete(root);
        cJSON_Delete(records);
        return -1;
    }
    text = cJSON_Print(root);
    rc = text && fprintf(out, "%s\n", text) >= 0 ? 0 : -1;
    free(text);
    cJSON_Delete(root);
    return rc;
}

unsigned long long
fmt_fields_mask(const char *fields, const struct fmt_field_def *defs, int count)
{
    unsigned long long mask = 0;
    int i;
    for (i = 0; i < count; i++)
        if (fmt_fields_requested(fields, defs[i].name) ||
            (defs[i].alias && fmt_fields_requested(fields, defs[i].alias)))
            mask |= defs[i].wire_mask;
    return mask;
}
