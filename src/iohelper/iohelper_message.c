/*
 * iohelper_message.c: Formatted messages between iohelper and us
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "iohelper_message.h"
#include "viralloc.h"
#include "virfile.h"
#include "virlog.h"
#include "virnetmessage.h"
#include "virobject.h"

#define VIR_FROM_THIS VIR_FROM_STREAMS

VIR_LOG_INIT("iohelperCtl");

struct iohelperCtl {
    virObject parent;

    int fd;
    bool blocking;
    virNetMessagePtr msg;
    bool msgReadyRead;
    bool msgReadyWrite;
    unsigned long long skipLength;
};

typedef ssize_t (*readfunc)(int fd, void *buf, size_t count);
typedef ssize_t (*writefunc)(int fd, const void *buf, size_t count);

static virClassPtr iohelperCtlClass;

static void
iohelperCtlDispose(void *obj)
{
    iohelperCtlPtr ctl = obj;

    virNetMessageFree(ctl->msg);
}

static int iohelperCtlOnceInit(void)
{
    if (!(iohelperCtlClass = virClassNew(virClassForObject(),
                                         "iohelperCtl",
                                         sizeof(iohelperCtl),
                                         iohelperCtlDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(iohelperCtl)

iohelperCtlPtr
iohelperCtlNew(int fd,
               bool blocking)
{
    iohelperCtlPtr ret;

    if (iohelperCtlInitialize() < 0)
        return NULL;

    if (!(ret = virObjectNew(iohelperCtlClass)))
        return NULL;

    if (!(ret->msg = virNetMessageNew(false)))
        goto error;

    ret->fd = fd;
    ret->blocking = blocking;
    ret->msgReadyRead = false;
    ret->msgReadyWrite = true;

    return ret;

 error:
    virObjectUnref(ret);
    return NULL;
}


static void
messageClear(iohelperCtlPtr ctl)
{
    virNetMessageClear(ctl->msg);
    ctl->msgReadyRead = false;
    ctl->msgReadyWrite = true;
}


static inline bool
messageReadyRead(iohelperCtlPtr ctl)
{
    return ctl->msgReadyRead;
}

static inline bool
messageReadyWrite(iohelperCtlPtr ctl)
{
    return ctl->msgReadyWrite;
}

static ssize_t
messageRecv(iohelperCtlPtr ctl)
{
    virNetMessagePtr msg = ctl->msg;
    readfunc readF = ctl->blocking ? saferead : read;
    virNetStreamSkip data;

    ctl->msgReadyRead = false;

    while (true) {
        ssize_t nread;
        size_t want;

        if (!msg->bufferLength) {
            msg->bufferLength = 4;
            if (VIR_ALLOC_N(msg->buffer, msg->bufferLength) < 0)
                return -1;
        }

        want = msg->bufferLength - msg->bufferOffset;

     reread:
        errno = 0;
        nread = readF(ctl->fd,
                      msg->buffer + msg->bufferOffset,
                      want);

        if (nread < 0) {
            if (errno == EINTR)
                goto reread;
            if (errno == EAGAIN)
                return 0;
            return -1;
        } else if (nread == 0) {
            /* EOF while reading */
            return 0;
        } else {
            msg->bufferOffset += nread;
        }

        if (msg->bufferOffset == msg->bufferLength) {
            if (msg->bufferOffset == 4) {
                if (virNetMessageDecodeLength(msg) < 0)
                    return -1;
            } else {
                if (virNetMessageDecodeHeader(msg) < 0)
                    return -1;

                if (msg->header.type == VIR_NET_STREAM_SKIP) {
                    if (virNetMessageDecodePayload(msg,
                                                   (xdrproc_t) xdr_virNetStreamSkip,
                                                   &data) < 0) {
                        return -1;
                    }

                    ctl->skipLength += data.length;
                    messageClear(ctl);
                    continue;
                }

                ctl->msgReadyRead = true;
                return msg->bufferLength - msg->bufferOffset;
            }
        }
    }
}


static ssize_t
messageSend(iohelperCtlPtr ctl)
{
    virNetMessagePtr msg = ctl->msg;
    writefunc writeF = ctl->blocking ? safewrite : write;

    ctl->msgReadyWrite = false;

    while (true) {
        ssize_t nwritten;
        size_t want;

        want = msg->bufferLength - msg->bufferOffset;

     rewrite:
        errno = 0;
        nwritten = writeF(ctl->fd,
                          msg->buffer + msg->bufferOffset,
                          want);

        if (nwritten < 0) {
            if (errno == EINTR)
                goto rewrite;
            if (errno == EAGAIN)
                return 0;
            return -1;
        } else if (nwritten == 0) {
            /* EOF while writing */
            return 0;
        } else {
            msg->bufferOffset += nwritten;
        }

        if (msg->bufferOffset == msg->bufferLength) {
            ctl->msgReadyWrite = true;
            return msg->bufferLength;
        }
    }
}


ssize_t
iohelperRead(iohelperCtlPtr ctl,
             char *bytes,
             size_t nbytes)
{
    ssize_t want = nbytes;
    virNetMessagePtr msg = ctl->msg;

    if (!messageReadyRead(ctl)) {
        ssize_t nread;
        /* Okay, the incoming message is not fully read. Try to
         * finish its receiving and recheck. */
        if ((nread = messageRecv(ctl)) < 0)
            return -1;

        if (!nread && errno != EAGAIN)
            return 0;

        if (!messageReadyRead(ctl)) {
            errno = EAGAIN;
            return -1;
        }
    }

    /* Should never happen, but things change. */
    if (msg->header.type != VIR_NET_STREAM) {
        errno = EAGAIN;
        return -1;
    }

    if (want > msg->bufferLength - msg->bufferOffset)
        want = msg->bufferLength - msg->bufferOffset;

    memcpy(bytes,
           msg->buffer + msg->bufferOffset,
           want);

    msg->bufferOffset += want;

    if (msg->bufferOffset == msg->bufferLength)
        messageClear(ctl);

    return want;
}


ssize_t
iohelperWrite(iohelperCtlPtr ctl,
              const char *bytes,
              size_t nbytes)
{
    size_t headerLen;
    ssize_t nwritten, totalNwritten = 0;

    virNetMessagePtr msg = ctl->msg;

    if (!messageReadyWrite(ctl)) {
        /* Okay, the outgoing message is not fully sent. Try to
         * finish the sending and recheck. */
        if ((nwritten = messageSend(ctl)) < 0)
            return -1;

        if (!nwritten && errno != EAGAIN)
            return 0;

        if (!messageReadyWrite(ctl)) {
            errno = EAGAIN;
            return -2;
        }

        totalNwritten += nwritten;
    }

    memset(&msg->header, 0, sizeof(msg->header));
    msg->header.type = VIR_NET_STREAM;
    msg->header.status = nbytes ? VIR_NET_CONTINUE : VIR_NET_OK;

    /* Encoding a message is fatal and we should discard any
     * partially encoded message. */
    if (virNetMessageEncodeHeader(msg) < 0)
        goto error;

    headerLen = msg->bufferOffset;

    if (virNetMessageEncodePayloadRaw(msg, bytes, nbytes) < 0)
        goto error;

    /* At this point, the message is successfully encoded. Don't
     * discard it if something below fails. */
    if ((nwritten = messageSend(ctl)) < 0)
        return -1;

    totalNwritten += nwritten - headerLen;

    return totalNwritten;

 error:
    messageClear(ctl);
    return -1;
}


int
iohelperSkip(iohelperCtlPtr ctl,
             unsigned long long length)
{
    virNetMessagePtr msg = ctl->msg;
    virNetStreamSkip data;

    if (messageReadyRead(ctl)) {
        /* This stream is used for reading. */
        return 0;
    }

    if (!messageReadyWrite(ctl)) {
        ssize_t nwritten;
        /* Okay, the outgoing message is not fully sent. Try to
         * finish the sending and recheck. */
        if ((nwritten = messageSend(ctl)) < 0)
            return -1;

        if (!nwritten && errno != EAGAIN)
            return 0;

        if (!messageReadyWrite(ctl)) {
            errno = EAGAIN;
            return -2;
        }
    }

    memset(&msg->header, 0, sizeof(msg->header));
    msg->header.type = VIR_NET_STREAM_SKIP;
    msg->header.status = VIR_NET_CONTINUE;

    memset(&data, 0, sizeof(data));
    data.length = length;

    /* Encoding a message is fatal and we should discard any
     * partially encoded message. */
    if (virNetMessageEncodeHeader(msg) < 0)
        goto error;

    if (virNetMessageEncodePayload(msg,
                                   (xdrproc_t) xdr_virNetStreamSkip,
                                   &data) < 0)
        goto error;

    /* At this point, the message is successfully encoded. Don't
     * discard it if something below fails. */
    if (messageSend(ctl) < 0)
        return -1;

    return 0;

 error:
    messageClear(ctl);
    return -1;
}


int
iohelperInData(iohelperCtlPtr ctl,
               int *inData,
               unsigned long long *length)
{
    virNetMessagePtr msg;

    /* Make sure we have a message waiting in the queue. */

    if (!messageReadyRead(ctl)) {
        ssize_t nread;
        /* Okay, the incoming message is not fully read. Try to
         * finish its receiving and recheck. */
        if ((nread = messageRecv(ctl)) < 0)
            return -1;

        if (!nread && errno != EAGAIN) {
            /* EOF */
            *inData = *length = 0;
            return 0;
        }

        if (!messageReadyRead(ctl)) {
            errno = EAGAIN;
            return -2;
        }
    }

    if (ctl->skipLength) {
        *inData = 0;
        *length = ctl->skipLength;
        ctl->skipLength = 0;
    } else {
        msg = ctl->msg;
        *inData = 1;
        *length = msg->bufferLength - msg->bufferOffset;
    }

    return 0;
}
