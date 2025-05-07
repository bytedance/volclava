#ifndef INCLUDE_LIB_SO_H
#define INCLUDE_LIB_SO_H

/* $Id: lib.so.h 397 2007-11-26 19:04:00Z mblack $
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


typedef int SO_HANDLE_T;


extern SO_HANDLE_T soOpen_(const char *libFileName);
extern void soClose_(SO_HANDLE_T handle);
extern void *soSym_(SO_HANDLE_T handle, const char *entryName);

#endif /* INCLUDE_LIB_SO_H */
