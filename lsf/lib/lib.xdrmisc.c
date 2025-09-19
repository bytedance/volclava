/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: lib.xdrmisc.c 397 2007-11-26 19:04:00Z mblack $
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

#include <limits.h>
#include "lib.h"
#include "lib.xdr.h"
#include "lproto.h"

bool_t
xdr_LSFlong (XDR *xdrs, long *l)
{
    struct {
        int high;
        int low;
    } longNum;

    if (xdrs->x_op == XDR_ENCODE) {
#if (LONG_BIT == 64)
        longNum.high = *l >> 32;
        longNum.low = *l & 0x00000000FFFFFFFF;
#else
        longNum.high = (*l < 0) ? -1 : 0;
        longNum.low = *l;
#endif
    }

    if (!(xdr_int(xdrs, &longNum.high) &&
          xdr_int(xdrs, &longNum.low))) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_ENCODE)
        return (TRUE);

#if (LONG_BIT == 64)
    *l = ((long) longNum.high) << 32;
    *l = *l | ((long) longNum.low & 0x00000000FFFFFFFF);
#else
    if (longNum.high > 0 ||
        (longNum.high == 0 && (longNum.low & 0x80000000)))
        *l = INT_MAX;
    else if (longNum.high < -1 ||
             (longNum.high == -1 && !(longNum.low & 0x80000000)))
        *l = INT_MIN;
    else
        *l = longNum.low;
#endif

    return (TRUE);
}

bool_t
xdr_stat(XDR *xdrs, struct stat *st, struct LSFHeader *hdr)
{
    int i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_dev;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_dev = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_ino;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_ino = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_mode;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_mode = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_nlink;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_nlink = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_uid;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_uid = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_gid;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_gid = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_rdev;

    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_rdev = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_size;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_size = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_atime;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_atime = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_mtime;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_mtime = i;

    if (xdrs->x_op == XDR_ENCODE)
        i = st->st_ctime;
    if (!xdr_int(xdrs, &i))
        return (FALSE);
    if (xdrs->x_op == XDR_DECODE)
        st->st_ctime = i;

    return (TRUE);
}

bool_t
xdr_lsfRusage(XDR *xdrs, struct lsfRusage *lsfRu)
{

    if (!(xdr_double(xdrs, &lsfRu->ru_utime) &&
          xdr_double(xdrs, &lsfRu->ru_stime) &&
          xdr_double(xdrs, &lsfRu->ru_maxrss) &&
          xdr_double(xdrs, &lsfRu->ru_ixrss) &&
          xdr_double(xdrs, &lsfRu->ru_ismrss) &&
          xdr_double(xdrs, &lsfRu->ru_idrss) &&
          xdr_double(xdrs, &lsfRu->ru_isrss) &&
          xdr_double(xdrs, &lsfRu->ru_minflt) &&
          xdr_double(xdrs, &lsfRu->ru_majflt) &&
          xdr_double(xdrs, &lsfRu->ru_nswap) &&
          xdr_double(xdrs, &lsfRu->ru_inblock) &&
          xdr_double(xdrs, &lsfRu->ru_oublock) &&
          xdr_double(xdrs, &lsfRu->ru_ioch) &&
          xdr_double(xdrs, &lsfRu->ru_msgsnd) &&
          xdr_double(xdrs, &lsfRu->ru_msgrcv) &&
          xdr_double(xdrs, &lsfRu->ru_nsignals) &&
          xdr_double(xdrs, &lsfRu->ru_nvcsw) &&
          xdr_double(xdrs, &lsfRu->ru_nivcsw) &&
          xdr_double(xdrs, &lsfRu->ru_exutime)))
        return (FALSE);
    return (TRUE);
}

bool_t
xdr_var_string(XDR *xdrs, char **astring)
{
    int pos;
    int len;

    if (xdrs->x_op == XDR_FREE) {
        FREEUP(*astring);
        return TRUE;
    }

    if (xdrs->x_op == XDR_DECODE) {

        pos = XDR_GETPOS(xdrs);
        *astring = NULL;

        if (!xdr_int(xdrs, &len)
            || ((*astring = calloc(len + 1, sizeof(char))) == NULL) )
            return FALSE;

        XDR_SETPOS(xdrs, pos);
    } else {
        if (*astring == NULL) {
           char *emptyStr = "";
           if (! xdr_string(xdrs, &emptyStr, 1)) {
               return FALSE;
           }          
           return TRUE;
        } else {
           len = strlen(*astring);
        }
    }

    if (! xdr_string(xdrs, astring, len + 1)) {
        if (xdrs->x_op == XDR_DECODE) {
            FREEUP(*astring);
        }
        return FALSE;
    }

    return TRUE;
}

bool_t
xdr_lenData(XDR *xdrs, struct lenData *ld)
{
    char *sp;

    if (!xdr_int(xdrs, &ld->len))
        return (FALSE);

    if (xdrs->x_op == XDR_FREE) {
        FREEUP(ld->data);
        return(TRUE);
    }

    if (ld->len == 0) {
        ld->data = NULL;
        return (TRUE);
    }

    if (xdrs->x_op == XDR_DECODE) {
        if ((ld->data = (char *) malloc(ld->len)) == NULL)
            return (FALSE);
    }

    sp = ld->data;
    if (!xdr_bytes(xdrs, &sp, (u_int *) &ld->len, ld->len)) {
        if (xdrs->x_op == XDR_DECODE)
            FREEUP(ld->data);
        return (FALSE);
    }

    return (TRUE);
}


bool_t
xdr_lsfAuth(XDR *xdrs, struct lsfAuth *auth, struct LSFHeader *hdr)
{

    char  *sp;

    sp = auth->lsfUserName;
    if (xdrs->x_op == XDR_DECODE)
        sp[0] = '\0';

    if (!(xdr_int(xdrs, &auth->uid) &&
          xdr_int(xdrs, &auth->gid) &&
          xdr_string(xdrs, &sp, MAXLSFNAMELEN)))
        return (FALSE);

    if (!xdr_enum(xdrs, (int *) &auth->kind))
        return (FALSE);

    switch (auth->kind) {
        case CLIENT_DCE:

            if (!xdr_int(xdrs, &auth->k.authToken.len))
                return (FALSE);

            if (xdrs->x_op == XDR_DECODE) {
                auth->k.authToken.data = (void *)malloc(auth->k.authToken.len);
                if (auth->k.authToken.data == NULL)
                    return (FALSE);
            }

            if (!xdr_bytes(xdrs,(char **)&auth->k.authToken.data,
                           (u_int *) &auth->k.authToken.len, auth->k.authToken.len))
                return (FALSE);

            break;

        case CLIENT_EAUTH:
            if (!xdr_int(xdrs, &auth->k.eauth.len))
                return (FALSE);

            sp = auth->k.eauth.data;
            if (!xdr_bytes(xdrs, &sp, (u_int *) &auth->k.eauth.len,
                           auth->k.eauth.len))
                return (FALSE);
            break;

        default:

            if (!xdr_arrayElement(xdrs, (char *) &auth->k.filler, hdr, xdr_int))
                return (FALSE);
            break;
    }



    if (xdrs->x_op == XDR_ENCODE) {
        auth->options = AUTH_HOST_UX;
    }

    if (!xdr_int(xdrs, &auth->options)) {
        return(FALSE);
    }


    return (TRUE);
}

bool_t
my_xdr_float(XDR *xdrs, float *fp)
{
    return(xdr_float(xdrs, fp));
}

int
xdr_lsfAuthSize(struct lsfAuth *auth)
{
    int sz = 0;

    if (auth == NULL)
        return (sz);

    sz += ALIGNWORD_(sizeof(auth->uid))
        + ALIGNWORD_(sizeof(auth->gid))
        + ALIGNWORD_(strlen(auth->lsfUserName))
        + ALIGNWORD_(sizeof(auth->kind));

    switch (auth->kind) {

        case CLIENT_DCE:
            sz += ALIGNWORD_(sizeof(auth->k.authToken.len))
                + ALIGNWORD_(auth->k.authToken.len);
            break;

        case CLIENT_EAUTH:
            sz += ALIGNWORD_(sizeof(auth->k.eauth.len))
                + ALIGNWORD_(auth->k.eauth.len);
            break;

        default:
            sz += ALIGNWORD_(sizeof(auth->k.filler));
    }
    sz += ALIGNWORD_(sizeof(auth->options));

    return (sz);
}

bool_t
xdr_pidInfo(XDR *xdrs, struct pidInfo *pidInfo, struct LSFHeader *hdr)
{

    if (! xdr_int(xdrs, &pidInfo->pid))
        return FALSE;
    if (! xdr_int(xdrs, &pidInfo->ppid))
        return FALSE;

    if (! xdr_int(xdrs, &pidInfo->pgid))
        return FALSE;

    if (! xdr_int(xdrs, &pidInfo->jobid))
        return FALSE;

    return (TRUE);
}

bool_t
xdr_jRusage (XDR *xdrs, struct jRusage *runRusage, struct LSFHeader *hdr)
{
    int i;

    if (xdrs->x_op == XDR_FREE) {
        FREEUP (runRusage->pidInfo);
        FREEUP (runRusage->pgid);
        return(TRUE);
    }

    if (xdrs->x_op == XDR_DECODE) {
        runRusage->pidInfo = NULL;
        runRusage->pgid = NULL;
    }

    if (!(xdr_int(xdrs, &runRusage->mem) &&
          xdr_int(xdrs, &runRusage->swap) &&
          xdr_int(xdrs, &runRusage->utime) &&
          xdr_int(xdrs, &runRusage->stime)))
        return (FALSE);



    if (!(xdr_int(xdrs, &runRusage->npids)))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE && runRusage->npids) {
        runRusage->pidInfo = calloc(runRusage->npids, sizeof(struct pidInfo));
        if (runRusage->pidInfo == NULL) {
            runRusage->npids = 0;
            return (FALSE);
        }
    }

    for (i = 0; i < runRusage->npids; i++) {
        if (!xdr_arrayElement(xdrs, (char *) &(runRusage->pidInfo[i]), hdr, xdr_pidInfo)) {
            if (xdrs->x_op == XDR_DECODE)  {
                FREEUP(runRusage->pidInfo);
                runRusage->npids = 0;
                runRusage->pidInfo = NULL;
            }
            return (FALSE);
        }
    }

    if (!(xdr_int(xdrs, &runRusage->npgids)))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE && runRusage->npgids) {
        runRusage->pgid = (int *) calloc (runRusage->npgids, sizeof(int));
        if (runRusage->pgid == NULL) {
            runRusage->npgids = 0;
            return(FALSE);
        }
    }

    for (i = 0; i < runRusage->npgids; i++) {

        if (! xdr_arrayElement(xdrs,
                               (char *)&(runRusage->pgid[i]),
                               hdr,
                               xdr_int)) {
            if (xdrs->x_op == XDR_DECODE) {
                FREEUP(runRusage->pgid);
                runRusage->npgids = 0;
                runRusage->pgid = NULL;
            }
            return (FALSE);
        }
    }
    return (TRUE);
}
