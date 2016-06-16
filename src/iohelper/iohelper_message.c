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
#include "virobject.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_STREAMS

VIR_LOG_INIT("iohelperCtl");

struct iohelperCtl {
    virObject parent;

    int fd;
};

static virClassPtr iohelperCtlClass;

static void
iohelperCtlDispose(void *obj)
{
    iohelperCtlPtr ctl = obj;

    VIR_DEBUG("obj = %p", ctl);
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
iohelperCtlNew(int fd)
{
    iohelperCtlPtr ret;

    if (iohelperCtlInitialize() < 0)
        return NULL;

    if (!(ret = virObjectNew(iohelperCtlClass)))
        return NULL;

    ret->fd = fd;

    return ret;
}


ssize_t
iohelperRead(iohelperCtlPtr ctl ATTRIBUTE_UNUSED,
             char *bytes ATTRIBUTE_UNUSED,
             size_t nbytes ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("sparse stream not supported"));
    return -1;
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
