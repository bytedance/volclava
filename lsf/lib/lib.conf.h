/* $Id: lib.conf.h 397 2007-11-26 19:04:00Z mblack $
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */
#ifndef LIB_CONF_H
#define LIB_CONF_H

#define ILLEGAL_CHARS     ".!-=+*/[]@:&|{}'`\""
#define M_THEN_A  1
#define A_THEN_M  2
#define M_OR_A    3
#define TYPE1  RESF_BUILTIN | RESF_DYNAMIC | RESF_GLOBAL
#define TYPE2  RESF_BUILTIN | RESF_GLOBAL
#define TYPE3  RESF_BUILTIN | RESF_GLOBAL | RESF_BATCH
#define DEF_REXPRIORITY 0

struct builtIn {
    char *name;
    char *des;
    enum valueType valueType;
    enum orderType orderType;
    int  flags;
    int  interval;
};

struct HostsArray {
    int  size;
    char** hosts;
};

void freeSA_(char**, int);

extern int builtInRes_ID[];
extern struct builtIn builtInRes[];

#endif
