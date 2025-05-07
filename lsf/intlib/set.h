#ifndef INCLUDE_SET_H
#define INCLUDE_SET_H

/* $Id: set.h 397 2007-11-26 19:04:00Z mblack $
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


struct listSet {
    int elem;             
    struct listSet *next;  
};

extern void listSetFree( struct listSet *);
extern struct listSet * listSetAlloc(int);
extern int listSetEqual (struct listSet *, struct listSet *);
extern struct listSet *listSetUnion (struct listSet *, struct listSet *);
extern struct listSet *listSetIntersect (struct listSet *, struct listSet *);
extern struct listSet *listSetDuplicate (struct listSet *);
extern int listSetIn (int, struct listSet *);
extern struct listSet *listSetInsert (int, struct listSet *);
extern struct listSet *listSetSub(struct listSet *, struct listSet *);

#endif /* INCLUDE_SET_H */


