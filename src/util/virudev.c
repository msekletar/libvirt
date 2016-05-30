/*
 * virudev.c: udev rules engine
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

#include "virudev.h"
#include "virobject.h"

struct _virUdevMgr {
    virObjectLockable parent;
};

static virClassPtr virUdevMgrClass;

static void
virUdevMgrDispose(void *obj ATTRIBUTE_UNUSED)
{
    /* nada */
}

static int virUdevMgrOnceInit(void)
{
    if (!(virUdevMgrClass = virClassNew(virClassForObjectLockable(),
                                        "virUdevMgr",
                                        sizeof(virUdevMgr),
                                        virUdevMgrDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virUdevMgr)

virUdevMgrPtr virUdevMgrNew(void)
{
    virUdevMgrPtr mgr;

    if (virUdevMgrInitialize() < 0)
        return NULL;

    if (!(mgr = virObjectLockableNew(virUdevMgrClass)))
        return NULL;

    return mgr;
}
