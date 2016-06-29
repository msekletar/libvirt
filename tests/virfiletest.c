/*
 * Copyright (C) 2013 Red Hat, Inc.
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef __linux__
# if HAVE_LINUX_MAGIC_H
#  include <linux/magic.h>
# endif
# include <sys/statfs.h>
#endif

#include "testutils.h"
#include "virfile.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_NONE


#if defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R
static int testFileCheckMounts(const char *prefix,
                               char **gotmounts,
                               size_t gotnmounts,
                               const char *const*wantmounts,
                               size_t wantnmounts)
{
    size_t i;
    if (gotnmounts != wantnmounts) {
        fprintf(stderr, "Expected %zu mounts under %s, but got %zu\n",
                wantnmounts, prefix, gotnmounts);
        return -1;
    }
    for (i = 0; i < gotnmounts; i++) {
        if (STRNEQ(gotmounts[i], wantmounts[i])) {
            fprintf(stderr, "Expected mount[%zu] '%s' but got '%s'\n",
                    i, wantmounts[i], gotmounts[i]);
            return -1;
        }
    }
    return 0;
}

struct testFileGetMountSubtreeData {
    const char *path;
    const char *prefix;
    const char *const *mounts;
    size_t nmounts;
    bool rev;
};

static int testFileGetMountSubtree(const void *opaque)
{
    int ret = -1;
    char **gotmounts = NULL;
    size_t gotnmounts = 0;
    const struct testFileGetMountSubtreeData *data = opaque;

    if (data->rev) {
        if (virFileGetMountReverseSubtree(data->path,
                                          data->prefix,
                                          &gotmounts,
                                          &gotnmounts) < 0)
            goto cleanup;
    } else {
        if (virFileGetMountSubtree(data->path,
                                   data->prefix,
                                   &gotmounts,
                                   &gotnmounts) < 0)
            goto cleanup;
    }

    ret = testFileCheckMounts(data->prefix,
                              gotmounts, gotnmounts,
                              data->mounts, data->nmounts);

 cleanup:
    virStringFreeList(gotmounts);
    return ret;
}
#endif /* ! defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R */

struct testFileSanitizePathData
{
    const char *path;
    const char *expect;
};

static int
testFileSanitizePath(const void *opaque)
{
    const struct testFileSanitizePathData *data = opaque;
    int ret = -1;
    char *actual;

    if (!(actual = virFileSanitizePath(data->path)))
        return -1;

    if (STRNEQ(actual, data->expect)) {
        fprintf(stderr, "\nexpect: '%s'\nactual: '%s'\n", data->expect, actual);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FREE(actual);
    return ret;
}

typedef struct {
    bool startData;
    unsigned long long *lengths;
    const char *dir;
    unsigned int fileno;
} seekTestData;


#ifndef EXT4_SUPER_MAGIC
# define EXT4_SUPER_MAGIC 0xef53
#endif
#ifndef XFS_SUPER_MAGIC
# define XFS_SUPER_MAGIC 0x58465342
#endif

static int
createSparseFile(int *retFD,
                 const char *filename,
                 unsigned int *blockSize,
                 bool startData,
                 unsigned long long *lengths)
{
    int ret = -1;
    int fd;
    bool inData = startData;
    struct statfs fs;
    size_t i;
    unsigned long long lengthSum = 0;
    char buf[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                  'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
                  'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
                  'y', 'z'};

    if ((fd = open(filename, O_CREAT|O_RDWR, 0666)) < 0) {
        fprintf(stderr, "Unable to create %s", filename);
        goto cleanup;
    }

    if (fstatfs(fd, &fs) < 0) {
        fprintf(stderr, "Unable to statfs %s", filename);
        goto cleanup;
    }

    /* Now, only some filesystems are known to work well with
     * SEEK_DATA and SEEK_HOLE. Proceed only with the tested
     * ones. */
    if (fs.f_type != EXT4_SUPER_MAGIC &&
        fs.f_type != XFS_SUPER_MAGIC) {
        *blockSize = 0;
        return 0;
    }

    *blockSize = fs.f_bsize;

    for (i = 0; lengths[i]; i++) {
        unsigned long long len = *blockSize * lengths[i];

        lengthSum += len;

        if (inData) {
            /* write @len bytes */
            while (len) {
                ssize_t nwritten;
                size_t want = len;
                if (want > sizeof(buf))
                    want = sizeof(buf);

                if ((nwritten = safewrite(fd, buf, want)) < 0) {
                    fprintf(stderr, "\nUnable to write %s\n", filename);
                    goto cleanup;
                }
                len -= nwritten;
            }
        } else {
            if (lseek(fd, len, SEEK_CUR) == (off_t) -1) {
                fprintf(stderr,
                        "\nUnable to seek %llu bytes in %s\n", len, filename);
                goto cleanup;
            }
        }

        inData = !inData;
    }

    if (ftruncate(fd, lengthSum) < 0) {
        fprintf(stderr, "\nUnable to truncate %s to %llu", filename, lengthSum);
        goto cleanup;
    }

    /* Now seek to the beginning of the file. */
    if (lseek(fd, 0, SEEK_SET) == (off_t) -1) {
        fprintf(stderr,
                "\nUnable to seek to the beginning of %s\n", filename);
        goto cleanup;
    }

    *retFD = fd;
    fd = -1;
    ret = 0;
 cleanup:
    VIR_FORCE_CLOSE(fd);
    return ret;
}


static int
testFileInData(const void *opaque)
{
    const seekTestData *data = opaque;
    char *filename = NULL;
    unsigned int blockSize;
    int ret = -1;
    int fd = -1;
    int realInData = data->startData, inData;
    unsigned long long len;
    size_t i;
    const off_t seekOffset = 64;

    if (virAsprintf(&filename, "%s/sparse-test-%u", data->dir, data->fileno) < 0)
        goto cleanup;

    if (createSparseFile(&fd, filename, &blockSize, data->startData, data->lengths) < 0)
        goto cleanup;

    if (!blockSize) {
        /* Underlying filesystem doesn't support SEEK_DATA and
         * SEEK_HOLE. Skip the test. */
        ret = EXIT_AM_SKIP;
        goto cleanup;
    }

    for (i = 0; data->lengths[i]; i++) {
        unsigned long long realLen = blockSize * data->lengths[i];

        while (realLen) {
            /* Check what the function thinks. */
            if (virFileInData(fd, &inData, &len) < 0) {
                fprintf(stderr, "\nvirFileInData failed\n");
                goto cleanup;
            }

            /* Now compare it with reality. */
            if (inData != realInData) {
                fprintf(stderr, "\nExpected inData = %d got = %d\n", realInData, inData);
                goto cleanup;
            }

            if (len != realLen) {
                fprintf(stderr, "\nExpected length = %lld got = %lld", realLen, len);
                goto cleanup;
            }

            /* And seek to next position. */
            if (lseek(fd, MIN(seekOffset, realLen), SEEK_CUR) == (off_t) -1) {
                fprintf(stderr, "\nUnable to seek in %s\n", filename);
                goto cleanup;
            }

            realLen -= MIN(seekOffset, realLen);
        }

        realInData = !realInData;
    }

    /* Here we are at the EOF. virFileInData should return:
     * inData = 0, len = 0. Check for that. */
    if (virFileInData(fd, &inData, &len) < 0) {
        fprintf(stderr, "\nvirFileInData failed\n");
        goto cleanup;
    }

    if (inData) {
        fprintf(stderr, "\nExpected inData = 0 got = %d\n", inData);
        goto cleanup;
    }

    if (len) {
        fprintf(stderr, "\nExpected length = 0 got = %lld", len);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    if (filename && !getenv("LIBVIRT_SKIP_CLEANUP"))
        unlink(filename);
    VIR_FREE(filename);
    VIR_FORCE_CLOSE(fd);
    return ret;
}


#define TESTDIRTEMPLATE abs_builddir "/virfiletestdir-XXXXXX"


static int
mymain(void)
{
    int ret = 0;
    struct testFileSanitizePathData data1;
    char *testdir;
    unsigned int fileno = 0;

    if (VIR_STRDUP_QUIET(testdir, TESTDIRTEMPLATE) < 0) {
        VIR_TEST_DEBUG("Out of memory\n");
        abort();
    }

    if (!mkdtemp(testdir)) {
        VIR_TEST_DEBUG("Cannot create fakerootdir");
        abort();
    }

#if defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R
# define MTAB_PATH1 abs_srcdir "/virfiledata/mounts1.txt"
# define MTAB_PATH2 abs_srcdir "/virfiledata/mounts2.txt"

    static const char *wantmounts1[] = {
        "/proc", "/proc/sys/fs/binfmt_misc", "/proc/sys/fs/binfmt_misc",
    };
    static const char *wantmounts1rev[] = {
        "/proc/sys/fs/binfmt_misc", "/proc/sys/fs/binfmt_misc", "/proc"
    };
    static const char *wantmounts2a[] = {
        "/etc/aliases"
    };
    static const char *wantmounts2b[] = {
        "/etc/aliases.db"
    };

# define DO_TEST_MOUNT_SUBTREE(name, path, prefix, mounts, rev)    \
    do {                                                           \
        struct testFileGetMountSubtreeData data = {                \
            path, prefix, mounts, ARRAY_CARDINALITY(mounts), rev   \
        };                                                         \
        if (virTestRun(name, testFileGetMountSubtree, &data) < 0)  \
            ret = -1;                                              \
    } while (0)

    DO_TEST_MOUNT_SUBTREE("/proc normal", MTAB_PATH1, "/proc", wantmounts1, false);
    DO_TEST_MOUNT_SUBTREE("/proc reverse", MTAB_PATH1, "/proc", wantmounts1rev, true);
    DO_TEST_MOUNT_SUBTREE("/etc/aliases", MTAB_PATH2, "/etc/aliases", wantmounts2a, false);
    DO_TEST_MOUNT_SUBTREE("/etc/aliases.db", MTAB_PATH2, "/etc/aliases.db", wantmounts2b, false);
#endif /* ! defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R */

#define DO_TEST_SANITIZE_PATH(PATH, EXPECT)                                    \
    do {                                                                       \
        data1.path = PATH;                                                     \
        data1.expect = EXPECT;                                                 \
        if (virTestRun(virTestCounterNext(), testFileSanitizePath,             \
                       &data1) < 0)                                            \
            ret = -1;                                                          \
    } while (0)

#define DO_TEST_SANITIZE_PATH_SAME(PATH) DO_TEST_SANITIZE_PATH(PATH, PATH)

    virTestCounterReset("testFileSanitizePath ");
    DO_TEST_SANITIZE_PATH("", "");
    DO_TEST_SANITIZE_PATH("/", "/");
    DO_TEST_SANITIZE_PATH("/path", "/path");
    DO_TEST_SANITIZE_PATH("/path/to/blah", "/path/to/blah");
    DO_TEST_SANITIZE_PATH("/path/", "/path");
    DO_TEST_SANITIZE_PATH("///////", "/");
    DO_TEST_SANITIZE_PATH("//", "//");
    DO_TEST_SANITIZE_PATH(".", ".");
    DO_TEST_SANITIZE_PATH("../", "..");
    DO_TEST_SANITIZE_PATH("../../", "../..");
    DO_TEST_SANITIZE_PATH("//foo//bar", "//foo/bar");
    DO_TEST_SANITIZE_PATH("/bar//foo", "/bar/foo");
    DO_TEST_SANITIZE_PATH_SAME("gluster://bar.baz/foo/hoo");
    DO_TEST_SANITIZE_PATH_SAME("gluster://bar.baz//fooo/hoo");
    DO_TEST_SANITIZE_PATH_SAME("gluster://bar.baz//////fooo/hoo");
    DO_TEST_SANITIZE_PATH_SAME("gluster://bar.baz/fooo//hoo");
    DO_TEST_SANITIZE_PATH_SAME("gluster://bar.baz/fooo///////hoo");

    /* Test virFileInData.
     * This test automatically creates the file and calls virFileInData over it.
     * @dataS: whether the file should start with data section.
     * @args: array of lengths of sections in blocks for each section.
     *
     * For instance: DO_SEEK_TEST(true, 1, 2, 3) will create test file with 1
     * block of data followed by 2 blocks of hole followed by 3 blocks of data.
     */
#define DO_SEEK_TEST(dataS, ...)                                        \
    do {                                                                \
        unsigned long long lengths[] = {__VA_ARGS__, 0};                \
        seekTestData data = {.dir = testdir, .fileno = ++fileno,        \
            .startData = dataS, .lengths = lengths};                    \
        if (virTestRun(virTestCounterNext(), testFileInData, &data) < 0)\
            ret = -1;                                                   \
    } while (0)

    virTestCounterReset("virFileInData ");
    DO_SEEK_TEST(true, 1, 2, 3);
    DO_SEEK_TEST(true, 1, 1, 1);
    DO_SEEK_TEST(false, 1, 2, 3);
    DO_SEEK_TEST(false, 1, 1, 1);

    if (getenv("LIBVIRT_SKIP_CLEANUP") == NULL)
        virFileDeleteTree(testdir);

    VIR_FREE(testdir);

    return ret != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

VIRT_TEST_MAIN(mymain)
