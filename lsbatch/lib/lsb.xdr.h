/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: lsb.xdr.h 397 2007-11-26 19:04:00Z mblack $
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
 

#include <stdio.h>
#include <rpc/types.h>
#ifndef __XDR_HEADER__
#include <rpc/xdr.h>
#endif
#include "../../lsf/lib/lproto.h"    

extern bool_t xdr_submitReq(XDR *, 
			    struct submitReq *, 
			    struct LSFHeader *);

extern bool_t xdr_submitMbdReply(XDR *, 
				 struct submitMbdReply *, 
				 struct LSFHeader *);

extern bool_t xdr_signalReq(XDR *, 
			    struct signalReq *, 
			    struct LSFHeader *);

extern bool_t xdr_lsbMsg(XDR *, 
			 struct lsbMsg *, 
			 struct LSFHeader *);

extern bool_t xdr_controlReq(XDR *,
			     struct controlReq *, 
			     struct LSFHeader *);

extern bool_t xdr_packSubmitReq(XDR *,
				 struct packSubmitReq *,
				 struct LSFHeader *);

extern bool_t xdr_debugReq (XDR *, 
			    struct debugReq *,
			    struct LSFHeader *);

extern bool_t xdr_infoReq(XDR *, 
			  struct infoReq *, 
			  struct LSFHeader *);

extern bool_t xdr_parameterInfo(XDR *, 
				struct parameterInfo *, 
				struct LSFHeader *);

extern bool_t xdr_userInfoEnt(XDR *, 
			       struct userInfoEnt *, 
			       struct LSFHeader *);

extern bool_t xdr_userInfoReply(XDR *, 
				struct userInfoReply *, 
				struct LSFHeader *);

extern bool_t xdr_hostInfoEnt(XDR *, 
			      struct hostInfoEnt *, 
			      struct LSFHeader *,
			      int *);

extern bool_t xdr_hostDataReply(XDR *, 
				struct hostDataReply *, 
				struct LSFHeader *);

extern bool_t xdr_queueInfoEnt(XDR *, 
			       struct queueInfoEnt *, 
			       struct LSFHeader *, 
			       int *);

extern bool_t xdr_queueInfoReply(XDR *, 
				 struct queueInfoReply *, 
				 struct LSFHeader *);

extern bool_t xdr_jobInfoHead(XDR *, 
			      struct jobInfoHead *, 
			      struct LSFHeader *);

extern bool_t xdr_jobInfoReply(XDR *, 
			       struct jobInfoReply *, 
			       struct LSFHeader *);

extern bool_t xdr_jobInfoEnt(XDR *, 
			     struct jobInfoEnt *, 
			     struct LSFHeader *);

extern bool_t xdr_jobInfoReq(XDR *, 
			     struct jobInfoReq *, 
			     struct LSFHeader *);

extern bool_t xdr_jobPeekReq(XDR *, 
			     struct jobPeekReq *, 
			     struct LSFHeader *);

extern bool_t xdr_jobPeekReply(XDR *, 
			       struct jobPeekReply *, 
			       struct LSFHeader *);

extern bool_t xdr_jobMoveReq(XDR *, 
			     struct jobMoveReq *, 
			     struct LSFHeader *);

extern bool_t xdr_jobSwitchReq(XDR *, 
			       struct jobSwitchReq *, 
			       struct LSFHeader *);

extern bool_t xdr_groupInfoReply(XDR *, 
				 struct groupInfoReply *, 
				 struct LSFHeader *);

extern bool_t xdr_groupInfoEnt(XDR *, 
			       struct groupInfoEnt *, 
			       struct LSFHeader *);

			       

extern bool_t xdr_migReq(XDR *, 
			 struct migReq *, 
			 struct LSFHeader *);

extern bool_t xdr_time_t(XDR *, 
			 time_t *);

extern bool_t xdr_xFile(XDR *, 
			struct xFile *, 
			struct LSFHeader *);

bool_t xdr_modifyReq(XDR *, 
		     struct  modifyReq *, 
		     struct LSFHeader *);

extern bool_t xdr_var_string(XDR *, 
			     char **);

extern bool_t xdr_lsbShareResourceInfoReply(XDR *,
					    struct  lsbShareResourceInfoReply *, 
					    struct LSFHeader *hdr);

extern bool_t xdr_runJobReq(XDR *, 
			    struct runJobRequest *, 
			    struct LSFHeader *);

extern bool_t xdr_jobAttrReq(XDR *, 
			    struct jobAttrInfoEnt *, 
			    struct LSFHeader *);

extern int  xdrsize_QueueInfoReply(struct queueInfoReply *);
