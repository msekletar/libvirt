/*
 * lock_daemon_seclabels.c: Security label mgmt
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
 * License along with this library;  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Michal Privoznik <mprivozn@redhat.com>
 */

#include <config.h>

#include "viralloc.h"
#include "virerror.h"
#include "virhash.h"
#include "virjson.h"
#include "virlog.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_RPC

VIR_LOG_INIT("locking.lock_daemon_dispatch");

#include "lock_daemon.h"
#include "lock_daemon_seclabels.h"

typedef struct _virSeclabel virSeclabel;
typedef virSeclabel *virSeclabelPtr;

struct _virSeclabel {
    char *model;
    char *label;
    unsigned int refcount;
};


typedef struct _virSeclabelSpaceLabels virSeclabelSpaceLabels;
typedef virSeclabelSpaceLabels *virSeclabelSpaceLabelsPtr;

struct _virSeclabelSpaceLabels {
    virSeclabelPtr *labels;
    size_t nlabels;
};

struct _virSeclabelSpace {
    virMutex lock;

    virHashTablePtr labels;
};


static void
virSeclabelFree(virSeclabelPtr label)
{
    if (!label)
        return;

    VIR_FREE(label->model);
    VIR_FREE(label->label);
    VIR_FREE(label);
}


static virSeclabelPtr
virSeclabelNew(const char *model,
               const char *label)
{
    virSeclabelPtr ret;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    if (VIR_STRDUP(ret->model, model) < 0 ||
        VIR_STRDUP(ret->label, label) < 0)
        goto error;

    ret->refcount = 1;

    return ret;
 error:
    virSeclabelFree(ret);
    return NULL;
}


static void
virSeclabelSpaceLabelsFree(virSeclabelSpaceLabelsPtr labels)
{
    size_t i;

    if (!labels)
        return;

    for (i = 0; i < labels->nlabels; i++)
        virSeclabelFree(labels->labels[i]);

    VIR_FREE(labels->labels);
    VIR_FREE(labels);
}

static void
virSeclabelSpaceLabelsHashFree(void *payload,
                               const void *name ATTRIBUTE_UNUSED)
{
    virSeclabelSpaceLabelsFree(payload);
}


static void
virSeclabelSpaceLock(virSeclabelSpacePtr space)
{
    virMutexLock(&space->lock);
}


static void
virSeclabelSpaceUnlock(virSeclabelSpacePtr space)
{
    virMutexUnlock(&space->lock);
}


void
virSeclabelSpaceFree(virSeclabelSpacePtr space)
{
    if (!space)
        return;

    virMutexDestroy(&space->lock);
    virHashFree(space->labels);
    VIR_FREE(space);
}


virSeclabelSpacePtr
virSeclabelSpaceNew(void)
{
    virSeclabelSpacePtr ret;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    if (virMutexInit(&ret->lock) < 0) {
        virReportSystemError(errno, "%s",
                     _("Unable to init mutex"));
        goto error;
    }

    if (!(ret->labels = virHashCreate(10, virSeclabelSpaceLabelsHashFree)))
        goto error;

    return ret;
 error:
    virSeclabelSpaceFree(ret);
    return NULL;
}


static virSeclabelPtr
virSeclabelSpaceLookup(virSeclabelSpacePtr space,
                       const char *path,
                       const char *model)
{
    virSeclabelSpaceLabelsPtr labels;
    virSeclabelPtr ret;
    size_t i;

    if (!(labels = virHashLookup(space->labels, path)))
        return NULL;

    for (i = 0; labels->nlabels; i++) {
        ret = labels->labels[i];

        if (STREQ(ret->model, model))
            return ret;
    }

    return NULL;
}


static int
virSeclabelSpaceAdd(virSeclabelSpacePtr space,
                    const char *path,
                    virSeclabelPtr label)
{
    virSeclabelSpaceLabelsPtr labels;
    virSeclabelPtr tmp;
    size_t i;
    int ret = -1;

    if (!(labels = virHashLookup(space->labels, path))) {
        /* Add new */
        if (VIR_ALLOC(labels) < 0 ||
            VIR_ALLOC(labels->labels) < 0 ||
            virHashAddEntry(space->labels, path, labels) < 0) {
            virSeclabelSpaceLabelsFree(labels);
            goto cleanup;
        }

        labels->labels[0] = label;
        labels->nlabels = 1;
    } else {
        /* Update old */
        for (i = 0; i < labels->nlabels; i++) {
            tmp = labels->labels[i];

            if (STREQ(tmp->model, label->model)) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("duplicate label for model '%s': old: '%s' new '%s'"),
                               tmp->model, tmp->label, label->label);
                goto cleanup;
            }
        }

        if (VIR_APPEND_ELEMENT_COPY(labels->labels, labels->nlabels, label) < 0)
            goto cleanup;

        if (virHashUpdateEntry(space->labels, path, labels) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    return ret;
}


static void
virSeclabelSpaceRemove(virSeclabelSpacePtr space,
                       const char *path,
                       virSeclabelPtr seclabel)
{
    virSeclabelSpaceLabelsPtr labels;
    virSeclabelPtr tmp;
    size_t i;

    if (!(labels = virHashLookup(space->labels, path))) {
        /* path not found */
        return;
    }

    for (i = 0; i < labels->nlabels; i++) {
        tmp = labels->labels[i];

        if (STREQ(seclabel->model, tmp->model))
            break;
    }

    if (i == labels->nlabels) {
        /* model not found */
        return;
    }

    tmp = labels->labels[i];

    VIR_DELETE_ELEMENT(labels->labels, i, labels->nlabels);

    if (!labels->nlabels)
        virHashRemoveEntry(space->labels, path);
}


struct virSeclabelSpaceIteratorData {
    virJSONValuePtr labels;
    bool error;
};


static void
virSeclabelSpaceIterator(void *payload,
                         const void *name,
                         void *opaque)
{
    virSeclabelSpaceLabelsPtr labels = payload;
    const char *path = name;
    struct virSeclabelSpaceIteratorData *data = opaque;
    virSeclabelPtr tmp;
    virJSONValuePtr item, jsonLabels, jsonLabel;
    size_t i;

    if (data->error)
        return;

    if (!(item = virJSONValueNewObject()))
        goto error;

    if (virJSONValueObjectAppendString(item, "path", path) < 0)
        goto error;

    if (!(jsonLabels = virJSONValueNewArray()))
        goto error;

    for (i = 0; i < labels->nlabels; i++) {
        tmp = labels->labels[i];

        if (!(jsonLabel = virJSONValueNewObject()))
            goto error;

        if (virJSONValueObjectAppendString(jsonLabel, "model", tmp->model) < 0 ||
            virJSONValueObjectAppendString(jsonLabel, "label", tmp->label) < 0 ||
            virJSONValueObjectAppendNumberUint(jsonLabel, "refcount", tmp->refcount) < 0)
            goto error;

        if (virJSONValueArrayAppend(jsonLabels, jsonLabel) < 0)
            goto error;
        jsonLabel = NULL;
    }

    if (virJSONValueObjectAppend(item, "labels", jsonLabels) < 0)
        goto error;
    jsonLabels = NULL;

    if (virJSONValueArrayAppend(data->labels, item) < 0)
        goto error;
    item = NULL;

    return;
 error:
    virJSONValueFree(jsonLabel);
    virJSONValueFree(jsonLabels);
    virJSONValueFree(item);
    data->error = true;
}


/**
 * virSeclabelSpacePreExecRestart:
 *
 * @space: object to dump
 *
 * Dumps virSeclabel object into JSON.
 *
 * Returns: a non-NULL pointer to JSON object on success,
 *          NULL on error
 */
virJSONValuePtr
virSeclabelSpacePreExecRestart(virSeclabelSpacePtr space)
{
    virJSONValuePtr object = NULL;
    virJSONValuePtr labels = NULL;
    struct virSeclabelSpaceIteratorData data;

    if (!space)
        return NULL;

    virSeclabelSpaceLock(space);

    if (!(object = virJSONValueNewObject()))
        return NULL;

    if (!(labels = virJSONValueNewArray()))
        goto error;

    data.labels = labels;
    data.error = false;
    if (virHashForEach(space->labels, virSeclabelSpaceIterator, &data) < 0)
        goto error;

    if (virJSONValueObjectAppend(object, "seclabels", labels) < 0)
        goto error;
    /* From now on, @labels is contained in @object. Avoid double freeing it. */
    labels = NULL;

    virSeclabelSpaceUnlock(space);
    return object;
 error:
    virSeclabelSpaceUnlock(space);
    virJSONValueFree(labels);
    virJSONValueFree(object);
    return NULL;
}


/**
 * virSeclabelSpacePostExecRestart:
 *
 * @object: JSON representation of internal state
 *
 * Read in JSON object, create new virSeclabelSpace object and restore its internal state from JSON.
 *
 * Returns: virSeclabelSpace object
 *          NULL on error
 */
virSeclabelSpacePtr
virSeclabelSpacePostExecRestart(virJSONValuePtr object)
{
    virSeclabelSpacePtr ret;
    virJSONValuePtr labels;
    size_t i, npaths;

    if (!(ret = virSeclabelSpaceNew()))
        return NULL;

    if (!(labels = virJSONValueObjectGetArray(object, "seclabels")))
        goto error;

    npaths = virJSONValueArraySize(labels);
    for (i = 0; i < npaths; i++) {
        virJSONValuePtr item = virJSONValueArrayGet(labels, i);
        virJSONValuePtr arr = virJSONValueObjectGetArray(item, "labels");
        const char *path = virJSONValueObjectGetString(item, "path");
        size_t j, nlabels;

        nlabels = virJSONValueArraySize(arr);

        for (j = 0; j < nlabels; j++) {
            virJSONValuePtr arr_item = virJSONValueArrayGet(arr, j);
            const char *model = virJSONValueObjectGetString(arr_item, "model");
            const char *label = virJSONValueObjectGetString(arr_item, "label");
            virSeclabelPtr seclabel;

            if (!(seclabel = virSeclabelNew(model, label)))
                goto error;

            if (virJSONValueObjectGetNumberUint(arr_item,
                                                "refcount",
                                                &seclabel->refcount) < 0) {
                virSeclabelFree(seclabel);
                goto error;
            }

            if (virSeclabelSpaceAdd(ret, path, seclabel) < 0) {
                virSeclabelFree(seclabel);
                goto error;
            }
        }
    }

    return ret;
 error:
    virSeclabelSpaceFree(ret);
    return NULL;
}


/**
 * virSeclabelSpaceRemember:
 *
 * @space: seclabel space object
 * @path: path to remember
 * @model: security model of @label ("selinux", "dac", etc.)
 * @label: actual value to hold (e.g. "root:root")
 *
 * This function should remember the original label for a @path on the first
 * call. Any subsequent call over the same @path and @model just increments the
 * refcounter for the label (actual value of @label is ignored in this case).
 *
 * Returns: 0 on success
 *          -1 otherwise
 */
int
virSeclabelSpaceRemember(virSeclabelSpacePtr space,
                         const char *path,
                         const char *model,
                         const char *label)
{
    virSeclabelPtr seclabel;
    int ret = -1;

    virSeclabelSpaceLock(space);
    if ((seclabel = virSeclabelSpaceLookup(space, path, model))) {
        /* We don't really care about label here. There already is an existing
         * record for the @path and @model, so @label is likely to not have the
         * original label anyway. */
        seclabel->refcount++;
        ret = 0;
        goto cleanup;
    }

    if (!(seclabel = virSeclabelNew(model, label)))
        goto cleanup;

    if (virSeclabelSpaceAdd(space, path, seclabel) < 0) {
        virSeclabelFree(seclabel);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    virSeclabelSpaceUnlock(space);
    return ret;
}


/**
 * virSeclabelSpaceRecall:
 *
 * @space: seclabel space object
 * @path: path to recall
 * @model: model to recall
 * @label: the original value remembered
 *
 * Counterpart for virSeclabelSpaceRemember. Upon successful return (retval ==
 * 0) @label is set to point to the original value remembered. The caller is
 * responsible for freeing the @label when no longer needed.
 *
 * Returns: 1 if label found, but still in use (refcount > 1), @label not touched
 *          0 if label found and not used anymore (refcount = 1), @label set
 *          -1 if no label was recorded with (@path, @model) tuple, @label not touched
 */
int
virSeclabelSpaceRecall(virSeclabelSpacePtr space,
                       const char *path,
                       const char *model,
                       char **label)
{
    int ret = -1;
    virSeclabelPtr seclabel;

    virSeclabelSpaceLock(space);

    if (!(seclabel = virSeclabelSpaceLookup(space, path, model)))
        goto cleanup;

    seclabel->refcount--;
    if (seclabel->refcount) {
        /* still in use */
        ret = 1;
        goto cleanup;
    }

    virSeclabelSpaceRemove(space, path, seclabel);
    /* steal the label pointer before freeing */
    *label = seclabel->label;
    seclabel->label = NULL;
    virSeclabelFree(seclabel);

    ret = 0;
 cleanup:
    virSeclabelSpaceUnlock(space);
    return ret;
}
