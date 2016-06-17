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
#include "virtime.h"

VIR_LOG_INIT("tests.iohelpermessagetest");

#define VIR_FROM_THIS VIR_FROM_NONE

typedef struct {
    const char * const *msg;
    unsigned int *len;
    bool blockR;
    bool blockW;
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

    if (testInit(ioCtl, pipeFD, data->blockR, data->blockW) < 0)
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

typedef struct {
    virMutexPtr lock;
    virCondPtr cond;
    bool finished;
    int ret;
    iohelperCtlPtr ioCtl;
    char *msg;
    size_t msgLen;
} threadData;

typedef threadData *threadDataPtr;

static void
readerThread(void *opaque)
{
    threadDataPtr data = opaque;
    char *bigBuf = NULL;
    size_t bigBufSize = 0;

    virObjectRef(data->ioCtl);
    /* Sleep some random time to simulate out of sync read &
     * write */
    usleep((rand() % 100) * 1000);

    while (true) {
        char buf[10]; /* Simulate reads of small chunks of data */
        ssize_t nread;

     reread:
        nread = iohelperRead(data->ioCtl, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EAGAIN) {
                usleep(20 * 1000);
                goto reread;
            }

            virFilePrintf(stderr, "Unable to read message (errno=%d)\n", errno);
            goto cleanup;
        }

        if (!nread)
            break;

        if (VIR_REALLOC_N(bigBuf, bigBufSize + nread) < 0)
            goto cleanup;

        memcpy(bigBuf + bigBufSize, buf, nread);
        bigBufSize += nread;
    }

    if (bigBufSize != data->msgLen) {
        virFilePrintf(stderr, "Message length mismatch: expected %zu got %zu",
                      data->msgLen, bigBufSize);
        goto cleanup;
    }

    if (memcmp(bigBuf, data->msg, data->msgLen)) {
        virFilePrintf(stderr, "Mismatched data");
        goto cleanup;
    }

    data->ret = 0;

 cleanup:
    VIR_FREE(bigBuf);
    virObjectUnref(data->ioCtl);
    virMutexLock(data->lock);
    data->finished = true;
    virCondSignal(data->cond);
    virMutexUnlock(data->lock);
}

static void
writerThread(void *opaque ATTRIBUTE_UNUSED)
{
    threadDataPtr data = opaque;
    size_t writeOff = 0;

    virObjectRef(data->ioCtl);
    /* Sleep some random time to simulate out of sync read &
     * write */
    usleep((rand() % 100) * 1000);

    while (true) {
        ssize_t nwritten;
        size_t want = data->msgLen - writeOff;

        if (!want)
            break;

     rewrite:
        nwritten = iohelperWrite(data->ioCtl,
                                 data->msg + writeOff,
                                 want);

        if (nwritten < 0) {
            if (errno == EAGAIN) {
                usleep(20 * 1000);
                goto rewrite;
            }

            virFilePrintf(stderr, "Unable to write message (errno=%d)\n", errno);
            goto cleanup;
        }

        if (!nwritten)
            break;

        writeOff += nwritten;
    }

    if (writeOff != data->msgLen) {
        virFilePrintf(stderr, "Message length mismatch: expected %zu written %zu",
                      data->msgLen, writeOff);
        goto cleanup;
    }

    data->ret = 0;

 cleanup:
    virObjectUnref(data->ioCtl);
    virMutexLock(data->lock);
    data->finished = true;
    virCondSignal(data->cond);
    virMutexUnlock(data->lock);
}

/* How long wait (in ms) for both reader & writer
 * threads to finish? */
#define WAIT_TIME 10000

static int
testNonblocking(const void *opaque)
{
    int ret = -1;
    const testData *data = opaque;
    iohelperCtlPtr ioCtl[2] = {NULL, NULL};
    int pipeFD[2] = {-1, -1};
    virThread reader, writer;
    threadData readerD, writerD;
    virMutex lock;
    virCond cond;
    unsigned long long now;
    unsigned long long then;
    char *msg = NULL;
    size_t msgLen = 0, idx;

    for (idx = 0; data->msg && data->msg[idx]; idx++) {
        const char *tmp = data->msg[idx];
        size_t tmpLen = strlen(tmp);

        if (VIR_REALLOC_N(msg, msgLen + tmpLen + 1) < 0)
            goto cleanup;

        memcpy(msg + msgLen, tmp, tmpLen + 1);
        msgLen += tmpLen;
    }

    for (idx = 0; data->len && data->len[idx]; idx++) {
        size_t tmpLen = data->len[idx];

        if (VIR_REALLOC_N(msg, msgLen + tmpLen) < 0)
            goto cleanup;
        msgLen += tmpLen;

        /* Here @msg contains some garbage that was on the heap
         * when the memory was allocated. That's okay, we want to
         * be sure iohelper can deal with binary garbage. */
    }

    if (virMutexInit(&lock) < 0 ||
        virCondInit(&cond) < 0)
        goto cleanup;

    if (testInit(ioCtl, pipeFD, data->blockR, data->blockW) < 0)
        goto cleanup;

    readerD = writerD = (threadData) {.lock = &lock, .cond = &cond,
        .ret = -1, .finished = false, .msg = msg, msgLen = msgLen};
    readerD.ioCtl = ioCtl[0];
    writerD.ioCtl = ioCtl[1];

    /* Now, ideally we would set the kernel's pipe buffer to be
     * small. Really small. Couple of bytes perhaps so that we
     * can be sure writes wrap around it just nicely. But the
     * smallest possible size is PAGESIZE. Trying to set anything
     * smaller than that is silently rounded up to PAGESIZE.
     * Okay, in that case we should write multiple of that. */
    fcntl(pipeFD[0], F_SETPIPE_SZ, 0);

    virMutexLock(&lock);

    if (virThreadCreate(&reader, false, readerThread, &readerD) < 0 ||
        virThreadCreate(&writer, false, writerThread, &writerD) < 0)
        goto cleanup;

    if (virTimeMillisNow(&now) < 0)
        goto cleanup;

    then = now + WAIT_TIME;

    while (!readerD.finished ||
           !writerD.finished) {
        if (virCondWaitUntil(&cond, &lock, then) < 0) {
            if (errno == ETIMEDOUT) {
                if (!readerD.finished)
                    virThreadCancel(&reader);
                if (!writerD.finished)
                    virThreadCancel(&writer);
            }

            goto cleanup;
        }
        if (readerD.finished)
            VIR_FORCE_CLOSE(pipeFD[0]);
        if (writerD.finished)
            VIR_FORCE_CLOSE(pipeFD[1]);
    }

    if (readerD.ret < 0 ||
        writerD.ret < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virMutexUnlock(&lock);
    virMutexDestroy(&lock);
    virCondDestroy(&cond);
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

    srand(time(NULL));

#define DO_TEST_BLOCKING_SIMPLE(...)                                \
    do {                                                            \
        const char *msg[] = { __VA_ARGS__, NULL};                   \
        testData data = {.blockR = true, .blockW = true,            \
            .msg = msg, .len = NULL };                              \
        if (virTestRun("Blocking simple", testBlocking, &data) < 0) \
        ret = -1;                                                   \
    } while (0)

#define DO_TEST_BLOCKING_LEN(...)                                   \
    do {                                                            \
        unsigned int len[] = { __VA_ARGS__, 0};                     \
        testData data = {.blockR = true, .blockW = true,            \
            .msg = NULL, .len = len };                              \
        if (virTestRun("Blocking len", testBlocking, &data) < 0)    \
        ret = -1;                                                   \
    } while (0)

#define DO_TEST_BLOCKR_SIMPLE(...)                                  \
    do {                                                            \
        const char *msg[] = { __VA_ARGS__, NULL};                   \
        testData data = {.blockR = true, .blockW = false,           \
            .msg = msg, .len = NULL };                              \
        if (virTestRun("Blocking read simple", testNonblocking, &data) < 0)    \
        ret = -1;                                                   \
    } while (0)

#define DO_TEST_BLOCKR_LEN(...)                                     \
    do {                                                            \
        unsigned int len[] = { __VA_ARGS__, 0};                     \
        testData data = {.blockR = true, .blockW = false,           \
            .msg = NULL, .len = len };                              \
        if (virTestRun("Blocking read len", testNonblocking, &data) < 0)    \
        ret = -1;                                                   \
    } while (0)

#define DO_TEST_BLOCKW_SIMPLE(...)                                  \
    do {                                                            \
        const char *msg[] = { __VA_ARGS__, NULL};                   \
        testData data = {.blockR = false, .blockW = true,           \
            .msg = msg, .len = NULL };                              \
        if (virTestRun("Blocking write simple", testNonblocking, &data) < 0)    \
        ret = -1;                                                   \
    } while (0)
#define DO_TEST_BLOCKW_LEN(...)                                     \
    do {                                                            \
        unsigned int len[] = { __VA_ARGS__, 0};                     \
        testData data = {.blockR = false, .blockW = true,           \
            .msg = NULL, .len = len };                              \
        if (virTestRun("Blocking write len", testNonblocking, &data) < 0)    \
        ret = -1;                                                   \
    } while (0)

    DO_TEST_BLOCKING_SIMPLE("Hello world");
    DO_TEST_BLOCKING_SIMPLE("Hello world", "Hello", "world");

    DO_TEST_BLOCKING_LEN(10);
    DO_TEST_BLOCKING_LEN(1024);
    DO_TEST_BLOCKING_LEN(32, 64, 128, 512, 1024);

    DO_TEST_BLOCKR_SIMPLE("Hello world");
    DO_TEST_BLOCKR_SIMPLE("Hello world", "Hello", "world");

    DO_TEST_BLOCKR_LEN(1024);
    DO_TEST_BLOCKR_LEN(409600);

    DO_TEST_BLOCKW_SIMPLE("Hello world");
    DO_TEST_BLOCKW_SIMPLE("Hello world", "Hello", "world");

    DO_TEST_BLOCKW_LEN(1024);
    DO_TEST_BLOCKW_LEN(409600);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIRT_TEST_MAIN(mymain)
