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

#include "internal.h"
#include "iohelper_message.h"
#include "viralloc.h"
#include "virerror.h"
#include "virfile.h"
#include "virnetmessage.h"

#define VIR_FROM_THIS VIR_FROM_STORAGE

struct _iohelperCtl {
    int fd;             /* FD used for read/write */
    virNetMessagePtr msg;
};

iohelperCtlPtr
iohelperCtlNew(int fd)
{
    iohelperCtlPtr ret;

    if (VIR_ALLOC(ret) < 0 ||
        !(ret->msg = virNetMessageNew(false)))
        goto error;

    ret->fd = fd;

    return ret;

 error:
    iohelperCtlFree(ret);
    return NULL;
}


void
iohelperCtlFree(iohelperCtlPtr ctl)
{
    if (!ctl)
        return;

    virNetMessageFree(ctl->msg);
    VIR_FREE(ctl);
}


int
iohelperCtlGetFD(iohelperCtlPtr ctl)
{
    if (!ctl)
        return -1;
    return ctl->fd;
}


static int
readOne(iohelperCtlPtr ctl,
        size_t len)
{
    ssize_t nread;
    virNetMessagePtr msg = ctl->msg;

    /* Couple of checks that should never happen. */
    if (msg->bufferLength) {
        /* The message is not yet fully processed. */
        return 0;
    }

    virNetMessageClear(msg);
    if (VIR_REALLOC_N(msg->buffer, len) < 0)
        return -1;

    nread = saferead(ctl->fd, msg->buffer, len);
    if (nread < 0) {
        virNetMessageClear(msg);
        virReportSystemError(errno, "%s", _("Unable to read from stream"));
        return -1;
    }

    msg->bufferLength = nread;
    return 0;
}


ssize_t
iohelperReadBuf(iohelperCtlPtr ctl,
                char *buf,
                size_t len)
{
    size_t want;
    virNetMessagePtr msg = ctl->msg;

    if (!msg->bufferLength &&
        readOne(ctl, len) < 0)
        return -1;

    want = msg->bufferLength - msg->bufferOffset;
    if (want > len)
        want = len;

    memcpy(buf,
           msg->buffer + msg->bufferOffset,
           want);
    msg->bufferOffset += want;

    if (msg->bufferOffset == msg->bufferLength)
        virNetMessageClear(msg);

    return want;
}


ssize_t
iohelperWriteBuf(iohelperCtlPtr ctl,
                 const char *buf,
                 size_t len)
{
    ssize_t nwritten, want;
    virNetMessagePtr msg = ctl->msg;

    if (len) {
        if (VIR_REALLOC_N(msg->buffer,
                          msg->bufferLength + len) < 0)
            return -1;

        msg->bufferLength += len;
        memcpy(msg->buffer + msg->bufferOffset,
               buf,
               len);
    }

    want = msg->bufferLength - msg->bufferOffset;
    nwritten = safewrite(ctl->fd,
                         msg->buffer + msg->bufferOffset,
                         want);
    if (nwritten < 0) {
        virReportSystemError(errno, "%s", _("Unable to write to stream"));
        return -1;
    }

    msg->bufferOffset += nwritten;
    return nwritten;
}


ssize_t
iohelperReadAsync(iohelperCtlPtr ctl)
{
    virNetMessagePtr msg = ctl->msg;
    size_t want = msg->bufferLength - msg->bufferOffset;
    ssize_t nread;



 retry:
    nread = read(ctl->fd,
                 msg->buffer + msg->bufferOffset,
                 want);
    if (nread < 0) {
        if (errno == EAGAIN) {
            return 0;
        } else if (errno == EINTR) {
            goto retry;
        }
        virReportSystemError(errno, "%s",
                             _("cannot read from stream"));

        return -1;
    }

    msg->bufferOffset += nread;
    return nread;
}


bool
iohelperReadAsyncCompleted(iohelperCtlPtr ctl)
{
    virNetMessagePtr msg = ctl->msg;

    return msg->bufferLength == msg->bufferOffset;
}
