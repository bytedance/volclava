/* $Id: lib.xdrlim.h 397 2007-11-26 19:04:00Z mblack $
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

#ifndef LIB_XDRLIM_H
#define LIB_XDRLIM_H

#include "lib.hdr.h"
#include "lproto.h"


extern bool_t xdr_decisionReq(XDR *,
                              struct decisionReq *,
                              struct LSFHeader *);
extern bool_t xdr_placeReply(XDR *,
                             struct placeReply *,
                             struct LSFHeader *);
extern bool_t xdr_loadReply(XDR *,
                            struct loadReply *,
                            struct LSFHeader *);
extern bool_t xdr_jobXfer(XDR *,
                          struct jobXfer *,
                          struct LSFHeader *);
extern bool_t xdr_hostInfo(XDR *,
                           struct shortHInfo *,
                           struct LSFHeader *);
extern bool_t xdr_limLock(XDR *,
                          struct limLock *,
                          struct LSFHeader *);
extern bool_t xdr_lsInfo(XDR *,
                         struct lsInfo *,
                         struct LSFHeader *);
extern bool_t xdr_hostInfoReply(XDR *,
                                struct hostInfoReply *,
                                struct LSFHeader *);
extern bool_t xdr_masterInfo(XDR *,
                             struct masterInfo *,
                             struct LSFHeader *);
extern bool_t xdr_clusterInfoReq(XDR *,
                                 struct clusterInfoReq *,
                                 struct LSFHeader *);
extern bool_t xdr_clusterInfoReply(XDR *,
                                   struct clusterInfoReply *,
                                   struct LSFHeader *);
extern bool_t xdr_shortHInfo(XDR *,
                             struct shortHInfo *, struct LSFHeader *,
                             char *);
extern bool_t xdr_shortCInfo(XDR *,
                             struct shortCInfo *,
                             struct LSFHeader *);
extern bool_t xdr_cInfo(XDR *,
                        struct cInfo *,
                        struct LSFHeader *);
extern bool_t xdr_resourceInfoReq(XDR *,
                                  struct resourceInfoReq *,
                                  struct LSFHeader *);
extern bool_t xdr_resourceInfoReply(XDR *,
                                    struct resourceInfoReply *,
                                    struct LSFHeader *);
extern bool_t xdr_hostEntry(XDR *,
                            struct hostEntry *,
                            struct LSFHeader *);
extern bool_t xdr_hostName(XDR *,
                           char *,
                           struct LSFHeader *);
extern bool_t xdr_hostOutputInfoReply(XDR *, struct hostInfoReply *, struct LSFHeader *);
#endif

