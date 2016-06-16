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
};

typedef ssize_t (*readfunc)(int fd, void *buf, size_t count);

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
}


static inline bool
messageReadyRead(iohelperCtlPtr ctl)
{
    return ctl->msgReadyRead;
}


static ssize_t
messageRecv(iohelperCtlPtr ctl)
{
    virNetMessagePtr msg = ctl->msg;
    readfunc readF = ctl->blocking ? saferead : read;

    ctl->msgReadyRead = false;

    if (!msg->bufferLength) {
        msg->bufferLength = 4;
        if (VIR_ALLOC_N(msg->buffer, msg->bufferLength) < 0)
            return -1;
    }

    while (true) {
        ssize_t nread;
        size_t want;

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

                /* Here we would decode the payload someday */

                ctl->msgReadyRead = true;
                return msg->bufferLength - msg->bufferOffset;
            }
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
iohelperWrite(iohelperCtlPtr ctl ATTRIBUTE_UNUSED,
              const char *bytes ATTRIBUTE_UNUSED,
              size_t nbytes ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("sparse stream not supported"));
    return -1;
}


int
iohelperSkip(iohelperCtlPtr ctl ATTRIBUTE_UNUSED,
             unsigned long long length ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("sparse stream not supported"));
    return -1;
}


int
iohelperInData(iohelperCtlPtr ctl ATTRIBUTE_UNUSED,
               int *inData ATTRIBUTE_UNUSED,
               unsigned long long *length ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("sparse stream not supported"));
    return -1;
}
