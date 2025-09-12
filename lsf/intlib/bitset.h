/* $Id: bitset.h 397 2007-11-26 19:04:00Z mblack $
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
#ifndef LS_BITSET_H
#define LS_BITSET_H

#include"../lib/lproto.h"
#include "list.h"

typedef struct _bitsetEvent         LS_BITSET_EVENT_T;


typedef struct {
    char *setDescription;
    unsigned int *bitmask;
    unsigned int setSize;
    unsigned int setWidth;
    unsigned int setNumElements;
    bool_t allowObservers;
    LIST_T *observers;
    int (*getIndexByObject)(void *);
    void *(*getObjectByIndex)(int);
} LS_BITSET_T;

typedef struct {
    LS_BITSET_T *set;
    unsigned int setCurrentBit;
    unsigned int setSize;
} LS_BITSET_ITERATOR_T;

typedef enum _bitsetEventType {
    LS_BITSET_EVENT_ENTER,
    LS_BITSET_EVENT_LEAVE,
    LS_BITSET_EVENT_NULL
} LS_BITSET_EVENT_TYPE_T;

struct _bitsetEvent {
    LS_BITSET_EVENT_TYPE_T     type;
    void                       *entry;
};

typedef bool_t  (*LS_BITSET_ENTRY_SELECT_OP_T)(void *extra,
					       LS_BITSET_EVENT_T *);


typedef int     (*LS_BITSET_EVENT_CALLBACK_FUNC_T)(LS_BITSET_T *set,
						   void *extra,
						   LS_BITSET_EVENT_T *event);

typedef struct _bitsetObserver{
    struct _bitsetObserver *            forw;
    struct _bitsetObserver *            back;
    char *                              name;
    LS_BITSET_T *                       set;
    void *                              extra;
    LS_BITSET_ENTRY_SELECT_OP_T         select;
    LS_BITSET_EVENT_CALLBACK_FUNC_T     enter;
    LS_BITSET_EVENT_CALLBACK_FUNC_T     leave_;
} LS_BITSET_OBSERVER_T;

#define BITSET_ITERATOR_ZERO_OUT(Iter) \
    memset((void *)(Iter), 0, sizeof(LS_BITSET_ITERATOR_T));

enum setSize {
    SET_SIZE_DEFAULT,
    SET_SIZE_CONST,
    SET_SIZE_VAR
};

enum bitState {
    SET_BIT_OFF,
    SET_BIT_ON
};


#define WORDLENGTH (sizeof(unsigned int)*8)

#define SET_DEFAULT_SIZE WORDLENGTH

#define SET_WORD_DEFAULT_EXTENT 2

#define LS_SET_UNION      0
#define LS_SET_INTERSECT  1
#define LS_SET_DIFFERENCE 2
#define LS_SET_ASSIGN     5

#define BYTES_IN_MASK(x) (x)*(sizeof(unsigned int))

#define SET_GET_WORD(position) (position/WORDLENGTH)

#define SET_GET_BIT_IN_WORD(position) (position % WORDLENGTH);

#define SET_IS_VALID(set) (set != NULL)

#define SET_IS_EMPTY(set) (set->setNumElements == 0)

#undef LS_BITSET_ERROR_CODE_ENTRY
#define LS_BITSET_ERROR_CODE_ENTRY(Id, Desc) Id,

extern int bitseterrno;

enum _lsBitSetErrno_ {
#    include "lsbitseterr.def"
     LS_BITSET_ERR_LAST
};

extern LS_BITSET_T *setCreate(const int, int (*getIndexByObject)(void *),
		 void *(*getObjectByIndex)(int), char *);
extern LS_BITSET_T *simpleSetCreate(const int, char *);
extern int setDestroy(LS_BITSET_T *);
extern LS_BITSET_T *setDup(const LS_BITSET_T *);
extern bool_t setTestValue(LS_BITSET_T *, const int);
extern int setGetSize(LS_BITSET_T *);
extern bool_t setIsMember(LS_BITSET_T *, void *);
extern int setAddElement(LS_BITSET_T *, void *);
extern int setRemoveElement(LS_BITSET_T *, void *);
extern int setClear(LS_BITSET_T *);
extern int setGetNumElements(LS_BITSET_T *);
extern void *setGetElement(LS_BITSET_T *, unsigned int);
extern LS_BITSET_ITERATOR_T *setIteratorCreate(LS_BITSET_T *);
extern int setIteratorAttach(LS_BITSET_ITERATOR_T *, LS_BITSET_T *, char *);
extern void setIteratorDetach(LS_BITSET_ITERATOR_T *);
extern void *setIteratorGetNextElement(LS_BITSET_ITERATOR_T *);
extern void *setIteratorBegin(LS_BITSET_ITERATOR_T *);
extern bool_t setIteratorIsEndOfSet(LS_BITSET_ITERATOR_T *);
extern void setIteratorDestroy(LS_BITSET_ITERATOR_T *);
extern bool_t setAllowObservers(LS_BITSET_T *);
extern LS_BITSET_OBSERVER_T *setObserverCreate(char *name, void *extra,
					 LS_BITSET_ENTRY_SELECT_OP_T select,
					 ...);
extern int setObserverAttach(LS_BITSET_OBSERVER_T *observer,
			     LS_BITSET_T *set);
extern int setNotifyObservers(LS_BITSET_T *set, LS_BITSET_EVENT_T *event);
extern int setDumpSet(LS_BITSET_T *, char *);
extern char *setPerror(int);
extern LS_BITSET_T *setEnlarge(LS_BITSET_T *, unsigned int);
extern void setOperate(LS_BITSET_T *, LS_BITSET_T *, int);
extern void setCat(LS_BITSET_T *, char *, int, char * (*)(void *, void *),
		    void *);

extern int  getNum1BitsInWord(unsigned int *word);

#endif
