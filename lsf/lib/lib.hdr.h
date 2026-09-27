/* $Id: lib.hdr.h 397 2007-11-26 19:04:00Z mblack $
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

#ifndef LIB_HDR_H
#define LIB_HDR_H

#include <rpc/types.h>
#ifndef __XDR_HEADER__
#include <rpc/xdr.h>
#endif
#include <sys/stat.h>

/* openlava 2.0 header breaks compatibility with
 * 1.0 but offers more flexibility and room for growth.
 */
struct LSFHeader {
    unsigned short refCode;
    unsigned short opCode;
    unsigned int length;
    unsigned short version;
    unsigned short reserved;
    unsigned int reserved0;
};

/* always use this macro to size up memory buffers
 * for protocol header.
 */
#define LSF_HEADER_LEN (sizeof(struct LSFHeader))

struct stringLen {
    char *name;
    int   len;
};

struct lenData {
    int len;
    char *data;
};

#define AUTH_HOST_NT  0x01
#define AUTH_HOST_UX  0x02

struct lsfAuth {
    int uid;
    int gid;
    char lsfUserName[MAXLSFNAMELEN];
    enum {CLIENT_SETUID, CLIENT_IDENT, CLIENT_DCE, CLIENT_EAUTH} kind;
    union authBody {
	int filler;
        struct lenData authToken;
	struct eauth {
#define EAUTH_SIZE 4096
	    int len;
	    char data[EAUTH_SIZE];
	} eauth;
    } k;
    int options;
};


struct lsfLimit {
    int rlim_curl;
    int rlim_curh;
    int rlim_maxl;
    int rlim_maxh;
};


extern bool_t xdr_LSFHeader(XDR *, struct LSFHeader *);
extern bool_t xdr_packLSFHeader(char *, struct LSFHeader *);

extern bool_t xdr_encodeMsg(XDR *, char *, struct LSFHeader *,
                            bool_t (*)(), int, struct lsfAuth *);
extern bool_t xdr_encodeMsgVersion(XDR *, char *, struct LSFHeader *,
                                   bool_t (*)(), int, struct lsfAuth *, int);

extern bool_t xdr_arrayElement(XDR *, char *, struct LSFHeader *,
				bool_t (*)(), ...);
extern bool_t xdr_LSFlong(XDR *, long *);
extern bool_t xdr_stringLen(XDR *, struct stringLen *, struct LSFHeader *);
extern bool_t xdr_stat(XDR *, struct stat *, struct LSFHeader *);
extern bool_t xdr_lsfAuth(XDR *, struct lsfAuth *, struct LSFHeader *);
extern int xdr_lsfAuthSize(struct lsfAuth *);
extern bool_t xdr_jRusage(XDR *, struct jRusage *, struct LSFHeader *);
#endif
