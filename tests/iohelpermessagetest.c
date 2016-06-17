/*
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
 * Author: Michal Privoznik <mprivozn@redhat.com>
 */

#include <config.h>

#include <fcntl.h>
#include <unistd.h>

#include "testutils.h"
#include "internal.h"
#include "iohelper_message.h"
#include "virlog.h"

VIR_LOG_INIT("tests.iohelpermessagetest");

#define VIR_FROM_THIS VIR_FROM_NONE

typedef struct {
    const char * const *msg;
    unsigned int *len;
} testData;

typedef testData *testDataPtr;

static int
testInit(iohelperCtlPtr ctl[2], int fd[2], bool blockR, bool blockW)
{
    ctl[0] = ctl[1] =  NULL;

    if (pipe(fd) < 0) {
        fprintf(stderr, "Cannot create pipe: %d", errno);
        return -1;
    }

    if (virSetBlocking(fd[0], blockR) < 0 ||
        virSetBlocking(fd[1], blockW) < 0)
        goto error;

    if (!(ctl[0] = iohelperCtlNew(fd[0], blockR)) ||
        !(ctl[1] = iohelperCtlNew(fd[1], blockW)))
        goto error;

    return 0;
 error:
    virObjectUnref(ctl[0]);
    virObjectUnref(ctl[1]);
    ctl[0] = ctl[1] =  NULL;
    return -1;
}

static int
testBlocking(const void *opaque)
{
    int ret = -1;
    const testData *data = opaque;
    iohelperCtlPtr ioCtl[2];
    int pipeFD[2] = {-1, -1};
    size_t idx = 0;
    bool quit = !data->msg && !data->len;
    char *genMsg = NULL;
    char buf[1024];

    if (testInit(ioCtl, pipeFD, true, true) < 0)
        goto cleanup;

    while (!quit) {
        const char *msg = NULL;
        size_t len = 0;
        ssize_t nread = 0, nwritten = 0;
        size_t i;

        if (data->len) {
            len = data->len[idx];
            quit = !data->len[idx + 1];
            VIR_FREE(genMsg);
            if (VIR_ALLOC_N(genMsg, len) < 0)
                goto cleanup;
            for (i = 0; i < len; i++)
                genMsg[i] = i;
            msg = genMsg;
            VIR_DEBUG("Testing string of len %zu", len);
        } else {
            msg = data->msg[idx];
            quit = !data->msg[idx + 1];
            len = strlen(msg);
            VIR_DEBUG("Testing string '%s'", msg);
        }

        if ((nwritten = iohelperWrite(ioCtl[1], msg, len)) < 0) {
            virFilePrintf(stderr, "Unable to write message (errno=%d)\n", errno);
            goto cleanup;
        }

        if (nwritten != len) {
            virFilePrintf(stderr, "Mismatched data len written=%zu wanted=%zu\n", nwritten, len);
            goto cleanup;
        }

        if ((nread = iohelperRead(ioCtl[0], buf, sizeof(buf))) < 0) {
            virFilePrintf(stderr, "Unable to read message (errno=%d)\n", errno);
            goto cleanup;
        }

        if (nread != nwritten) {
            virFilePrintf(stderr, "Mismatched data len written=%zu read=%zu\n", nwritten, nread);
            goto cleanup;
        }

        buf[nread] = '\0';

        if (memcmp(buf, msg, nread)) {
            virFilePrintf(stderr, "Mismatched data written='%s' read='%s'\n", msg, buf);
            goto cleanup;
        }

        idx++;
    }

    ret = 0;
 cleanup:
    VIR_FREE(genMsg);
    virObjectUnref(ioCtl[0]);
    virObjectUnref(ioCtl[1]);
    VIR_FORCE_CLOSE(pipeFD[0]);
    VIR_FORCE_CLOSE(pipeFD[1]);
    return ret;
}

static int
mymain(void)
{
    int ret = 0;

#define DO_TEST_BLOCKING_SIMPLE(...)                                \
    do {                                                            \
        const char *msg[] = { __VA_ARGS__, NULL};                   \
        testData data = {.msg = msg, .len = NULL };                 \
        if (virTestRun("Blocking simple", testBlocking, &data) < 0) \
        ret = -1;                                                   \
    } while (0)

#define DO_TEST_BLOCKING_LEN(...)                                   \
    do {                                                            \
        unsigned int len[] = { __VA_ARGS__, 0};                     \
        testData data = {.msg = NULL, .len = len };                 \
        if (virTestRun("Blocking len", testBlocking, &data) < 0)    \
        ret = -1;                                                   \
    } while (0)

    DO_TEST_BLOCKING_SIMPLE("Hello world");
    DO_TEST_BLOCKING_SIMPLE("Hello world", "Hello", "world");

    DO_TEST_BLOCKING_LEN(10);
    DO_TEST_BLOCKING_LEN(1024);
    DO_TEST_BLOCKING_LEN(32, 64, 128, 512, 1024);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIRT_TEST_MAIN(mymain)
