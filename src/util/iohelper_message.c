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
#include "virerror.h"
#include "virfile.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_NONE
VIR_LOG_INIT("util.ioheler_message");


void
iohelperFree(iohelperMessagePtr msg)
{
    if (!msg)
        return;

    VIR_FREE(msg);
}


static ssize_t
iohelperReadPlain(const char *fdName, int fd, size_t buflen, iohelperMessagePtr *msg)
{
    ssize_t got;

    if (VIR_ALLOC(*msg) < 0)
        goto error;

    if (buflen > BUFSIZE)
        buflen = BUFSIZE;

    got = saferead(fd, (*msg)->data.buf.buf, buflen);
    if (got < 0) {
        virReportSystemError(errno, _("Unable to read %s"), fdName);
        goto error;
    }

    (*msg)->type = IOHELPER_MESSAGE_DATA;
    (*msg)->data.buf.buflen = got;

    return got;

 error:
    iohelperFree(*msg);
    *msg = NULL;
    return -1;
}


static ssize_t
iohelperWritePlain(const char *fdName, int fd, iohelperMessagePtr msg)
{
    if (msg->type == IOHELPER_MESSAGE_DATA) {
        ssize_t written = safewrite(fd, msg->data.buf.buf, msg->data.buf.buflen);
        if (written < 0) {
            virReportSystemError(errno, _("Unable to write %s"), fdName);
            return -1;
        }
        return written;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Unknown message type: %d"), msg->type);
    return -1;
}


static bool
iohelperMessageValid(iohelperMessagePtr msg)
{
    if (!msg)
        return true;

    if (msg->type != IOHELPER_MESSAGE_DATA) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown message type %d"), msg->type);
        return false;
    }

    return true;
}


static ssize_t
iohelperReadFormatted(const char *fdName, int fd, iohelperMessagePtr *msg)
{
    char *buf;
    ssize_t want, offset = 0, got = 0;

    if (VIR_ALLOC(*msg) < 0)
        goto error;

    buf = (char *) *msg;
    want = sizeof(**msg);
    while (offset < want) {
        got = saferead(fd, buf + offset, want);
        if (got < 0) {
            virReportSystemError(errno, _("Unable to read %s"), fdName);
            goto error;
        }

        want -= got;
        offset += got;
    }

    /* Now that we read the message, we should validate it. */
    if (!iohelperMessageValid(*msg))
        goto error;

    return (*msg)->data.buf.buflen;

 error:
    iohelperFree(*msg);
    *msg = NULL;
    return -1;
}


static ssize_t
iohelperWriteFormatted(const char *fdName, int fd, iohelperMessagePtr msg)
{
    char *buf;
    ssize_t want, offset = 0, got = 0;

    if (!iohelperMessageValid(msg))
        return -1;

    buf = (char *) msg;
    want = sizeof(*msg);
    while (offset < want) {
        got = safewrite(fd, buf + offset, want);
        if (got < 0) {
            virReportSystemError(errno, _("Unable to write %s"), fdName);
            return -1;
        }

        want -= got;
        offset += got;
    }

    return msg->data.buf.buflen;
}


ssize_t
iohelperRead(const char *fdName, int fd, size_t buflen,
             iohelperMessagePtr *msg, bool formatted)
{
    if (formatted)
        return iohelperReadFormatted(fdName, fd, msg);
    else
        return iohelperReadPlain(fdName, fd, buflen, msg);
}


ssize_t
iohelperWrite(const char *fdName, int fd, iohelperMessagePtr msg, bool formatted)
{
    if (formatted)
        return iohelperWriteFormatted(fdName, fd, msg);
    else
        return iohelperWritePlain(fdName, fd, msg);
}
