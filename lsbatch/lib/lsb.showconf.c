/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 */

#include "lsb.h"
#include "../../lsf/lib/lib.daemoninfo.h"

/*
 * Request showconf data from a batch daemon.
 * callmbd() routes MBD requests to QMBD when configured and falls back to MBD.
 * SBD requests use the target host's command channel.
 * @param[in] daemon: SHOWCONF_MBD or SHOWCONF_SBD
 * @param[in] host: Target host for SHOWCONF_SBD; NULL for SHOWCONF_MBD
 * @param[out] reply: Decoded showconf reply; caller must free it with
 *                    freeShowConfReply(reply, FALSE)
 * @return: 0 on success, -1 on validation, transport, or decode failure
 */
int
lsb_showconf(int daemon, char *host, struct showConfReply *reply)
{
    char request_buf[MSGSIZE];
    char *reply_buf = NULL;
    struct LSFHeader hdr;
    XDR xdrs;
    int opCode;
    int cc;

    if (reply == NULL || (daemon == SHOWCONF_SBD && host == NULL)) {
        lsberrno = LSBE_BAD_ARG;
        return -1;
    }

    /* Translate the public daemon selector into the daemon request opcode. */
    if (daemon == SHOWCONF_MBD)
        opCode = BATCH_SHOWCONF;
    else if (daemon == SHOWCONF_SBD)
        opCode = CMD_SBD_SHOWCONF;
    else {
        lsberrno = LSBE_BAD_ARG;
        return -1;
    }

    memset(reply, 0, sizeof(*reply));
    initLSFHeader_(&hdr);
    hdr.opCode = opCode;

    xdrmem_create(&xdrs, request_buf, sizeof(request_buf), XDR_ENCODE);
    if (!xdr_encodeMsg(&xdrs, NULL, &hdr, NULL, 0, NULL)) {
        xdr_destroy(&xdrs);
        lsberrno = LSBE_XDR;
        return -1;
    }

    if (host == NULL)
        cc = callmbd(NULL, request_buf, XDR_GETPOS(&xdrs), &reply_buf,
                     &hdr, NULL, NULL, NULL);
    else
        cc = cmdCallSBD_(host, request_buf, XDR_GETPOS(&xdrs), &reply_buf,
                         &hdr, NULL);
    xdr_destroy(&xdrs);

    if (cc < 0)
        return -1;

    /* The reply header opcode carries the daemon-side LSBE_* status. */
    lsberrno = hdr.opCode;
    if (hdr.opCode != LSBE_NO_ERROR) {
        if (cc)
            FREEUP(reply_buf);
        return -1;
    }

    /* Decode into caller-owned storage after the daemon reports success. */
    xdrmem_create(&xdrs, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);
    if (!xdr_showConfReply(&xdrs, reply, &hdr)) {
        xdr_destroy(&xdrs);
        freeShowConfReply(reply, FALSE);
        if (cc)
            FREEUP(reply_buf);
        lsberrno = LSBE_XDR;
        return -1;
    }
    xdr_destroy(&xdrs);

    if (cc)
        FREEUP(reply_buf);
    return 0;
}
