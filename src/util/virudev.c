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

#include <libudev.h>

#include "virudev.h"
#include "virobject.h"

#define VIR_FROM_THIS VIR_FROM_NONE

struct _virUdevMgr {
    virObjectLockable parent;

    struct udev *udev;
};

static virClassPtr virUdevMgrClass;

static void
virUdevMgrDispose(void *obj)
{
    virUdevMgrPtr mgr = obj;

    udev_unref(mgr->udev);
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

    if (!(mgr->udev = udev_new()))
        goto error;

    return mgr;

 error:
    virObjectUnref(mgr);
    return NULL;
}

static struct udev_device *
virUdevMgrFindDevice(virUdevMgrPtr mgr,
                     const char *path)
{
    if (STRPREFIX(path, "/dev/")) {
        struct stat statbuf;
        char type;

        if (stat(path, &statbuf) < 0)
            return NULL;

        if (S_ISBLK(statbuf.st_mode)) {
            type = 'b';
        } else if (S_ISCHR(statbuf.st_mode)) {
            type = 'c';
        } else {
            virReportError(VIR_ERR_NO_NODE_DEVICE,
                           _("no node device with matching name '%s'"),
                           path);
            return NULL;
        }

        return udev_device_new_from_devnum(mgr->udev, type, statbuf.st_rdev);
    } else if (STRPREFIX(path, "/sys/")) {
        return udev_device_new_from_syspath(mgr->udev, path);
    } else {
        virReportError(VIR_ERR_NO_NODE_DEVICE,
                       _("no node device with matching name '%s'"),
                       path);
        return NULL;
    }
}

int
virUdevMgrAddLabel(virUdevMgrPtr mgr,
                   const char *path)
{
    int ret = -1;
    struct udev_device *device;

    if (!(device = virUdevMgrFindDevice(mgr, path)))
        return ret;

}

int
virUdevMgrRemoveLabel(virUdevMgrPtr mgr,
                      const char *device)
{
}
