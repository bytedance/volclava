/* $Id: lib.xdr.h 397 2007-11-26 19:04:00Z mblack $
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

#ifndef LIB_XDR_H
#define LIB_XDR_H
#include "lib.hdr.h"

extern bool_t xdr_time_t (XDR *, time_t *);
extern bool_t xdr_lsfRusage(XDR *, struct lsfRusage *);
extern bool_t xdr_lvector(XDR *, float *, int);
extern bool_t xdr_array_string(XDR *, char **, int, int);
extern bool_t xdr_var_string(XDR *, char **);
extern bool_t xdr_stringLen(XDR *, struct stringLen *, struct LSFHeader *);
extern bool_t xdr_lenData(XDR *, struct lenData *);
extern bool_t xdr_lsfLimit (XDR *, struct lsfLimit *, struct LSFHeader *);
extern bool_t xdr_portno (XDR *, u_short *);
extern bool_t xdr_address (XDR *, u_int *);
extern int getXdrStrlen (char *);

#endif
