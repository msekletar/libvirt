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


static int
iohelperInData(const char *fdName,
               int fd,
               bool *inData,
               unsigned long long *length)
{
    int ret = -1;
    off_t cur, data, hole;

    *inData = 0;
    *length = 0;

    /* Get current position */
    cur = lseek(fd, 0, SEEK_CUR);
    if (cur == (off_t) -1) {
        virReportSystemError(errno,
                             _("Unable to get current position in %s"), fdName);
        goto cleanup;
    }

    /* Now try to get data and hole offsets */
    data = lseek(fd, cur, SEEK_DATA);

    /* There are four options:
     * 1) data == cur;  @cur is in data
     * 2) data > cur; @cur is in a hole, next data at @data
     * 3) data < 0, errno = ENXIO; either @cur is in trailing hole, or @cur is beyond EOF.
     * 4) data < 0, errno != ENXIO; we learned nothing
     */

    if (data == (off_t) -1) {
        /* cases 3 and 4 */
        if (errno != ENXIO) {
            virReportSystemError(errno,
                                 _("Unable to seek to data in %s"), fdName);
            goto cleanup;
        }
        *inData = false;
        *length = 0;
    } else if (data > cur) {
        /* case 2 */
        *inData = false;
        *length = data - cur;
    } else {
        /* case 1 */
        *inData = true;

        /* We don't know where does the next hole start. Let's
         * find out. Here we get the same 4 possibilities as
         * described above.*/
        hole = lseek(fd, data, SEEK_HOLE);
        if (hole == (off_t) -1 || hole == data) {
            /* cases 1, 3 and 4 */
            /* Wait a second. The reason why we are here is
             * because we are in data. But at the same time we
             * are in a trailing hole? Wut!? Do the best what we
             * can do here. */
            virReportSystemError(errno,
                                 _("unable to seek to hole in %s"), fdName);
            goto cleanup;
        } else {
            /* case 2 */
            *length = (hole - data);
        }
    }

    ret = 0;
 cleanup:
    /* If we were in data initially, reposition back. */
    if (*inData && cur != (off_t) -1)
        ignore_value(lseek(fd, cur, SEEK_SET));
    return ret;
}


static ssize_t
iohelperReadPlain(const char *fdName, int fd, size_t buflen, iohelperMessagePtr *msg)
{
    ssize_t got;
    bool inData;
    unsigned long long length;

    if (iohelperInData(fdName, fd, &inData, &length) < 0)
        goto error;

    if (VIR_ALLOC(*msg) < 0)
        goto error;

    if (inData && length) {
        if (buflen > BUFSIZE)
            buflen = BUFSIZE;

        if (buflen > length)
            buflen = length;

        got = saferead(fd, (*msg)->data.buf.buf, buflen);
        if (got < 0) {
            virReportSystemError(errno, _("Unable to read %s"), fdName);
            goto error;
        }

        (*msg)->type = IOHELPER_MESSAGE_DATA;
        (*msg)->data.buf.buflen = got;
    } else {
        (*msg)->type = IOHELPER_MESSAGE_HOLE;
        got = (*msg)->data.length = length;
    }

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
    } else if (msg->type == IOHELPER_MESSAGE_HOLE) {
        if (lseek(fd, msg->data.length, SEEK_CUR) == (off_t) -1) {
            virReportSystemError(errno, _("Unable to seek %s"), fdName);
            return -1;
        }

        return msg->data.length;
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

    if (msg->type != IOHELPER_MESSAGE_DATA &&
        msg->type != IOHELPER_MESSAGE_HOLE) {
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
