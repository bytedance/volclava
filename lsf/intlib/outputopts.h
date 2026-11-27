/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * Copyright (C) 2007 Platform Computing Inc
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 *
 */

#ifndef VOLCLAVA_OUTPUTOPTS_H
#define VOLCLAVA_OUTPUTOPTS_H

#include <getopt.h>
#include <string.h>

enum { OPT_NOHEADER = 256 };

static inline int
getopt_output(int argc, char **argv, const char *shortopts)
{
    static const struct option longopts[] = {
        {"noheader", no_argument, NULL, OPT_NOHEADER},
        {NULL, 0, NULL, 0}
    };
    int option = getopt_long_only(argc, argv, shortopts, longopts, NULL);

    /* getopt_long_only also recognizes abbreviations and double dashes.
     * Accept only the documented single-dash spelling.  Let getopt consume
     * option operands itself, so e.g. -J -noheader still names a job. */
    if (option == OPT_NOHEADER && strcmp(argv[optind - 1], "-noheader") != 0)
        return '?';
    return option;
}

#endif
