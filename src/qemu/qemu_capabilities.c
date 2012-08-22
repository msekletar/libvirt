/*
 * qemu_capabilities.c: QEMU capabilities generation
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include "qemu_capabilities.h"
#include "memory.h"
#include "logging.h"
#include "virterror_internal.h"
#include "util.h"
#include "virfile.h"
#include "nodeinfo.h"
#include "cpu/cpu.h"
#include "domain_conf.h"
#include "qemu_conf.h"
#include "command.h"
#include "virnodesuspend.h"

#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <stdarg.h>

#define VIR_FROM_THIS VIR_FROM_QEMU

/* While not public, these strings must not change. They
 * are used in domain status files which are read on
 * daemon restarts
 */
VIR_ENUM_IMPL(qemuCaps, QEMU_CAPS_LAST,
              "kqemu",  /* 0 */
              "vnc-colon",
              "no-reboot",
              "drive",
              "drive-boot",

              "name", /* 5 */
              "uuid",
              "domid",
              "vnet-hdr",
              "migrate-kvm-stdio",

              "migrate-qemu-tcp", /* 10 */
              "migrate-qemu-exec",
              "drive-cache-v2",
              "kvm",
              "drive-format",

              "vga", /* 15 */
              "0.10",
              "pci-device",
              "mem-path",
              "drive-serial",

              "xen-domid", /* 20 */
              "migrate-qemu-unix",
              "chardev",
              "enable-kvm",
              "monitor-json",

              "balloon", /* 25 */
              "device",
              "sdl",
              "smp-topology",
              "netdev",

              "rtc", /* 30 */
              "vhost-net",
              "rtc-td-hack",
              "no-hpet",
              "no-kvm-pit",

              "tdf", /* 35 */
              "pci-configfd",
              "nodefconfig",
              "boot-menu",
              "enable-kqemu",

              "fsdev", /* 40 */
              "nesting",
              "name-process",
              "drive-readonly",
              "smbios-type",

              "vga-qxl", /* 45 */
              "spice",
              "vga-none",
              "migrate-qemu-fd",
              "boot-index",

              "hda-duplex", /* 50 */
              "drive-aio",
              "pci-multibus",
              "pci-bootindex",
              "ccid-emulated",

              "ccid-passthru", /* 55 */
              "chardev-spicevmc",
              "device-spicevmc",
              "virtio-tx-alg",
              "device-qxl-vga",

              "pci-multifunction", /* 60 */
              "virtio-blk-pci.ioeventfd",
              "sga",
              "virtio-blk-pci.event_idx",
              "virtio-net-pci.event_idx",

              "cache-directsync", /* 65 */
              "piix3-usb-uhci",
              "piix4-usb-uhci",
              "usb-ehci",
              "ich9-usb-ehci1",

              "vt82c686b-usb-uhci", /* 70 */
              "pci-ohci",
              "usb-redir",
              "usb-hub",
              "no-shutdown",

              "cache-unsafe", /* 75 */
              "rombar",
              "ich9-ahci",
              "no-acpi",
              "fsdev-readonly",

              "virtio-blk-pci.scsi", /* 80 */
              "blk-sg-io",
              "drive-copy-on-read",
              "cpu-host",
              "fsdev-writeout",

              "drive-iotune", /* 85 */
              "system_wakeup",
              "scsi-disk.channel",
              "scsi-block",
              "transaction",

              "block-job-sync", /* 90 */
              "block-job-async",
              "scsi-cd",
              "ide-cd",
              "no-user-config",

              "hda-micro", /* 95 */
              "dump-guest-memory",
              "nec-usb-xhci",
              "virtio-s390",
              "balloon-event",

              "bridge", /* 100 */
              "lsi",
              "virtio-scsi-pci",
              "blockio",
              "disable-s3",

              "disable-s4", /* 105 */
              "usb-redir.filter",
    );

struct _qemuCaps {
    virObject object;

    virBitmapPtr flags;

    unsigned int version;
    unsigned int kvmVersion;

    char *arch;

    size_t ncpuDefinitions;
    char **cpuDefinitions;

    size_t nmachineTypes;
    char **machineTypes;
    char **machineAliases;
};


static virClassPtr qemuCapsClass;
static void qemuCapsDispose(void *obj);

static int qemuCapsOnceInit(void)
{
    if (!(qemuCapsClass = virClassNew("qemuCaps",
                                      sizeof(qemuCaps),
                                      qemuCapsDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuCaps)

struct qemu_feature_flags {
    const char *name;
    const int default_on;
    const int toggle;
};

struct qemu_arch_info {
    const char *arch;
    int wordsize;
    const char *machine;
    const char *binary;
    const char *altbinary;
    const struct qemu_feature_flags *flags;
    int nflags;
};

/* Feature flags for the architecture info */
static const struct qemu_feature_flags const arch_info_i686_flags [] = {
    { "pae",  1, 0 },
    { "nonpae",  1, 0 },
    { "acpi", 1, 1 },
    { "apic", 1, 0 },
};

static const struct qemu_feature_flags const arch_info_x86_64_flags [] = {
    { "acpi", 1, 1 },
    { "apic", 1, 0 },
};

/* The archicture tables for supported QEMU archs */
static const struct qemu_arch_info const arch_info_hvm[] = {
    {  "i686",   32, NULL, "qemu",
       "qemu-system-x86_64", arch_info_i686_flags, 4 },
    {  "x86_64", 64, NULL, "qemu-system-x86_64",
       NULL, arch_info_x86_64_flags, 2 },
    {  "arm",    32, NULL, "qemu-system-arm",    NULL, NULL, 0 },
    {  "microblaze", 32, NULL, "qemu-system-microblaze",   NULL, NULL, 0 },
    {  "microblazeel", 32, NULL, "qemu-system-microblazeel",   NULL, NULL, 0 },
    {  "mips",   32, NULL, "qemu-system-mips",   NULL, NULL, 0 },
    {  "mipsel", 32, NULL, "qemu-system-mipsel", NULL, NULL, 0 },
    {  "sparc",  32, NULL, "qemu-system-sparc",  NULL, NULL, 0 },
    {  "ppc",    32, NULL, "qemu-system-ppc",    NULL, NULL, 0 },
    {  "ppc64",    64, NULL, "qemu-system-ppc64",    NULL, NULL, 0 },
    {  "itanium", 64, NULL, "qemu-system-ia64",  NULL, NULL, 0 },
    {  "s390x",  64, NULL, "qemu-system-s390x",  NULL, NULL, 0 },
};

static const struct qemu_arch_info const arch_info_xen[] = {
    {  "i686",   32, "xenner", "xenner", NULL, arch_info_i686_flags, 4 },
    {  "x86_64", 64, "xenner", "xenner", NULL, arch_info_x86_64_flags, 2 },
};


static virCommandPtr
qemuCapsProbeCommand(const char *qemu,
                     qemuCapsPtr caps)
{
    virCommandPtr cmd = virCommandNew(qemu);

    if (caps) {
        if (qemuCapsGet(caps, QEMU_CAPS_NO_USER_CONFIG))
            virCommandAddArg(cmd, "-no-user-config");
        else if (qemuCapsGet(caps, QEMU_CAPS_NODEFCONFIG))
            virCommandAddArg(cmd, "-nodefconfig");
    }

    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);

    return cmd;
}


/* Format is:
 * <machine> <desc> [(default)|(alias of <canonical>)]
 */
static int
qemuCapsParseMachineTypesStr(const char *output,
                             virCapsGuestMachinePtr **machines,
                             size_t *nmachines)
{
    const char *p = output;
    const char *next;
    virCapsGuestMachinePtr *list = NULL;
    int nitems = 0;

    do {
        const char *t;
        virCapsGuestMachinePtr machine;

        if ((next = strchr(p, '\n')))
            ++next;

        if (STRPREFIX(p, "Supported machines are:"))
            continue;

        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (VIR_ALLOC(machine) < 0)
            goto no_memory;

        if (!(machine->name = strndup(p, t - p))) {
            VIR_FREE(machine);
            goto no_memory;
        }

        if (VIR_REALLOC_N(list, nitems + 1) < 0) {
            VIR_FREE(machine->name);
            VIR_FREE(machine);
            goto no_memory;
        }

        p = t;
        if (!(t = strstr(p, "(default)")) || (next && t >= next)) {
            list[nitems++] = machine;
        } else {
            /* put the default first in the list */
            memmove(list + 1, list, sizeof(*list) * nitems);
            list[0] = machine;
            nitems++;
        }

        if ((t = strstr(p, "(alias of ")) && (!next || t < next)) {
            p = t + strlen("(alias of ");
            if (!(t = strchr(p, ')')) || (next && t >= next))
                continue;

            if (!(machine->canonical = strndup(p, t - p)))
                goto no_memory;
        }
    } while ((p = next));

    *machines = list;
    *nmachines = nitems;

    return 0;

  no_memory:
    virReportOOMError();
    virCapabilitiesFreeMachines(list, nitems);
    return -1;
}

int
qemuCapsProbeMachineTypes(const char *binary,
                          qemuCapsPtr caps,
                          virCapsGuestMachinePtr **machines,
                          size_t *nmachines)
{
    char *output;
    int ret = -1;
    virCommandPtr cmd;
    int status;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(binary)) {
        virReportSystemError(errno, _("Cannot find QEMU binary %s"), binary);
        return -1;
    }

    cmd = qemuCapsProbeCommand(binary, caps);
    virCommandAddArgList(cmd, "-M", "?", NULL);
    virCommandSetOutputBuffer(cmd, &output);

    /* Ignore failure from older qemu that did not understand '-M ?'.  */
    if (virCommandRun(cmd, &status) < 0)
        goto cleanup;

    if (qemuCapsParseMachineTypesStr(output, machines, nmachines) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);

    return ret;
}

static int
qemuCapsGetOldMachinesFromInfo(virCapsGuestDomainInfoPtr info,
                               const char *emulator,
                               time_t emulator_mtime,
                               virCapsGuestMachinePtr **machines,
                               size_t *nmachines)
{
    virCapsGuestMachinePtr *list;
    int i;

    if (!info->nmachines)
        return 0;

    if (!info->emulator || !STREQ(emulator, info->emulator))
        return 0;

    if (emulator_mtime != info->emulator_mtime) {
        VIR_DEBUG("mtime on %s has changed, refreshing machine types",
                  info->emulator);
        return 0;
    }

    if (VIR_ALLOC_N(list, info->nmachines) < 0) {
        virReportOOMError();
        return 0;
    }

    for (i = 0; i < info->nmachines; i++) {
        if (VIR_ALLOC(list[i]) < 0) {
            goto no_memory;
        }
        if (info->machines[i]->name &&
            !(list[i]->name = strdup(info->machines[i]->name))) {
            goto no_memory;
        }
        if (info->machines[i]->canonical &&
            !(list[i]->canonical = strdup(info->machines[i]->canonical))) {
            goto no_memory;
        }
    }

    *machines = list;
    *nmachines = info->nmachines;

    return 1;

  no_memory:
    virReportOOMError();
    virCapabilitiesFreeMachines(list, info->nmachines);
    return 0;
}

static int
qemuCapsGetOldMachines(const char *ostype,
                       const char *arch,
                       int wordsize,
                       const char *emulator,
                       time_t emulator_mtime,
                       virCapsPtr old_caps,
                       virCapsGuestMachinePtr **machines,
                       size_t *nmachines)
{
    int i;

    for (i = 0; i < old_caps->nguests; i++) {
        virCapsGuestPtr guest = old_caps->guests[i];
        int j;

        if (!STREQ(ostype, guest->ostype) ||
            !STREQ(arch, guest->arch.name) ||
            wordsize != guest->arch.wordsize)
            continue;

        for (j = 0; j < guest->arch.ndomains; j++) {
            virCapsGuestDomainPtr dom = guest->arch.domains[j];

            if (qemuCapsGetOldMachinesFromInfo(&dom->info,
                                               emulator, emulator_mtime,
                                               machines, nmachines))
                return 1;
        }

        if (qemuCapsGetOldMachinesFromInfo(&guest->arch.defaultInfo,
                                           emulator, emulator_mtime,
                                           machines, nmachines))
            return 1;
    }

    return 0;
}


typedef int
(*qemuCapsParseCPUModels)(const char *output,
                          size_t *retcount,
                          const char ***retcpus);

/* Format:
 *      <arch> <model>
 * qemu-0.13 encloses some model names in []:
 *      <arch> [<model>]
 */
static int
qemuCapsParseX86Models(const char *output,
                       size_t *retcount,
                       const char ***retcpus)
{
    const char *p = output;
    const char *next;
    unsigned int count = 0;
    const char **cpus = NULL;
    int i;

    do {
        const char *t;

        if ((next = strchr(p, '\n')))
            next++;

        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (!STRPREFIX(p, "x86"))
            continue;

        p = t;
        while (*p == ' ')
            p++;

        if (*p == '\0' || *p == '\n')
            continue;

        if (retcpus) {
            unsigned int len;

            if (VIR_REALLOC_N(cpus, count + 1) < 0) {
                virReportOOMError();
                goto error;
            }

            if (next)
                len = next - p - 1;
            else
                len = strlen(p);

            if (len > 2 && *p == '[' && p[len - 1] == ']') {
                p++;
                len -= 2;
            }

            if (!(cpus[count] = strndup(p, len))) {
                virReportOOMError();
                goto error;
            }
        }
        count++;
    } while ((p = next));

    if (retcount)
        *retcount = count;
    if (retcpus)
        *retcpus = cpus;

    return 0;

error:
    if (cpus) {
        for (i = 0; i < count; i++)
            VIR_FREE(cpus[i]);
    }
    VIR_FREE(cpus);

    return -1;
}

/* ppc64 parser.
 * Format : PowerPC <machine> <description>
 */
static int
qemuCapsParsePPCModels(const char *output,
                       size_t *retcount,
                       const char ***retcpus)
{
    const char *p = output;
    const char *next;
    unsigned int count = 0;
    const char **cpus = NULL;
    int i, ret = -1;

    do {
        const char *t;

        if ((next = strchr(p, '\n')))
            next++;

        if (!STRPREFIX(p, "PowerPC "))
            continue;

        /* Skip the preceding sub-string "PowerPC " */
        p += 8;

        /*Malformed string, does not obey the format 'PowerPC <model> <desc>'*/
        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (*p == '\0')
            break;

        if (*p == '\n')
            continue;

        if (retcpus) {
            unsigned int len;

            if (VIR_REALLOC_N(cpus, count + 1) < 0) {
                virReportOOMError();
                goto cleanup;
            }

            len = t - p - 1;

            if (!(cpus[count] = strndup(p, len))) {
                virReportOOMError();
                goto cleanup;
            }
        }
        count++;
    } while ((p = next));

    if (retcount)
        *retcount = count;
    if (retcpus) {
        *retcpus = cpus;
        cpus = NULL;
    }
    ret = 0;

cleanup:
    if (cpus) {
        for (i = 0; i < count; i++)
            VIR_FREE(cpus[i]);
        VIR_FREE(cpus);
    }
    return ret;
}

int
qemuCapsProbeCPUModels(const char *qemu,
                       qemuCapsPtr caps,
                       const char *arch,
                       size_t *count,
                       const char ***cpus)
{
    char *output = NULL;
    int ret = -1;
    qemuCapsParseCPUModels parse;
    virCommandPtr cmd;

    if (count)
        *count = 0;
    if (cpus)
        *cpus = NULL;

    if (STREQ(arch, "i686") || STREQ(arch, "x86_64"))
        parse = qemuCapsParseX86Models;
    else if (STREQ(arch, "ppc64"))
        parse = qemuCapsParsePPCModels;
    else {
        VIR_DEBUG("don't know how to parse %s CPU models", arch);
        return 0;
    }

    cmd = qemuCapsProbeCommand(qemu, caps);
    virCommandAddArgList(cmd, "-cpu", "?", NULL);
    virCommandSetOutputBuffer(cmd, &output);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (parse(output, count, cpus) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);

    return ret;
}


static int
qemuCapsInitGuest(virCapsPtr caps,
                  virCapsPtr old_caps,
                  const char *hostmachine,
                  const struct qemu_arch_info *info,
                  int hvm)
{
    virCapsGuestPtr guest;
    int i;
    int haskvm = 0;
    int haskqemu = 0;
    char *kvmbin = NULL;
    char *binary = NULL;
    time_t binary_mtime;
    virCapsGuestMachinePtr *machines = NULL;
    size_t nmachines = 0;
    struct stat st;
    size_t ncpus;
    qemuCapsPtr qemubinCaps = NULL;
    qemuCapsPtr kvmbinCaps = NULL;
    int ret = -1;

    /* Check for existance of base emulator, or alternate base
     * which can be used with magic cpu choice
     */
    binary = virFindFileInPath(info->binary);

    if (binary == NULL || !virFileIsExecutable(binary)) {
        VIR_FREE(binary);
        binary = virFindFileInPath(info->altbinary);
    }

    /* Ignore binary if extracting version info fails */
    if (binary &&
        qemuCapsExtractVersionInfo(binary, info->arch,
                                   false, NULL, &qemubinCaps) < 0)
        VIR_FREE(binary);

    /* qemu-kvm/kvm binaries can only be used if
     *  - host & guest arches match
     * Or
     *  - hostarch is x86_64 and guest arch is i686
     * The latter simply needs "-cpu qemu32"
     */
    if (STREQ(info->arch, hostmachine) ||
        (STREQ(hostmachine, "x86_64") && STREQ(info->arch, "i686"))) {
        const char *const kvmbins[] = { "/usr/libexec/qemu-kvm", /* RHEL */
                                        "qemu-kvm", /* Fedora */
                                        "kvm" }; /* Upstream .spec */

        for (i = 0; i < ARRAY_CARDINALITY(kvmbins); ++i) {
            kvmbin = virFindFileInPath(kvmbins[i]);

            if (!kvmbin)
                continue;

            if (qemuCapsExtractVersionInfo(kvmbin, info->arch,
                                           false, NULL,
                                           &kvmbinCaps) < 0) {
                VIR_FREE(kvmbin);
                continue;
            }

            if (!binary) {
                binary = kvmbin;
                qemubinCaps = kvmbinCaps;
                kvmbin = NULL;
                kvmbinCaps = NULL;
            }
            break;
        }
    }

    if (!binary)
        return 0;

    if (access("/dev/kvm", F_OK) == 0 &&
        (qemuCapsGet(qemubinCaps, QEMU_CAPS_KVM) ||
         qemuCapsGet(qemubinCaps, QEMU_CAPS_ENABLE_KVM) ||
         kvmbin))
        haskvm = 1;

    if (access("/dev/kqemu", F_OK) == 0 &&
        qemuCapsGet(qemubinCaps, QEMU_CAPS_KQEMU))
        haskqemu = 1;

    if (stat(binary, &st) == 0) {
        binary_mtime = st.st_mtime;
    } else {
        char ebuf[1024];
        VIR_WARN("Failed to stat %s, most peculiar : %s",
                 binary, virStrerror(errno, ebuf, sizeof(ebuf)));
        binary_mtime = 0;
    }

    if (info->machine) {
        virCapsGuestMachinePtr machine;

        if (VIR_ALLOC(machine) < 0) {
            goto no_memory;
        }

        if (!(machine->name = strdup(info->machine))) {
            VIR_FREE(machine);
            goto no_memory;
        }

        nmachines = 1;

        if (VIR_ALLOC_N(machines, nmachines) < 0) {
            VIR_FREE(machine->name);
            VIR_FREE(machine);
            goto no_memory;
        }

        machines[0] = machine;
    } else {
        int probe = 1;
        if (old_caps && binary_mtime)
            probe = !qemuCapsGetOldMachines(hvm ? "hvm" : "xen", info->arch,
                                            info->wordsize, binary, binary_mtime,
                                            old_caps, &machines, &nmachines);
        if (probe &&
            qemuCapsProbeMachineTypes(binary, qemubinCaps,
                                      &machines, &nmachines) < 0)
            goto error;
    }

    /* We register kvm as the base emulator too, since we can
     * just give -no-kvm to disable acceleration if required */
    if ((guest = virCapabilitiesAddGuest(caps,
                                         hvm ? "hvm" : "xen",
                                         info->arch,
                                         info->wordsize,
                                         binary,
                                         NULL,
                                         nmachines,
                                         machines)) == NULL)
        goto error;

    machines = NULL;
    nmachines = 0;

    guest->arch.defaultInfo.emulator_mtime = binary_mtime;

    if (caps->host.cpu &&
        qemuCapsProbeCPUModels(binary, NULL, info->arch, &ncpus, NULL) == 0 &&
        ncpus > 0 &&
        !virCapabilitiesAddGuestFeature(guest, "cpuselection", 1, 0))
        goto error;

    if (qemuCapsGet(qemubinCaps, QEMU_CAPS_BOOTINDEX) &&
        !virCapabilitiesAddGuestFeature(guest, "deviceboot", 1, 0))
        goto error;

    if (hvm) {
        if (virCapabilitiesAddGuestDomain(guest,
                                          "qemu",
                                          NULL,
                                          NULL,
                                          0,
                                          NULL) == NULL)
            goto error;

        if (haskqemu &&
            virCapabilitiesAddGuestDomain(guest,
                                          "kqemu",
                                          NULL,
                                          NULL,
                                          0,
                                          NULL) == NULL)
            goto error;

        if (haskvm) {
            virCapsGuestDomainPtr dom;

            if (kvmbin) {
                int probe = 1;

                if (stat(kvmbin, &st) == 0) {
                    binary_mtime = st.st_mtime;
                } else {
                    char ebuf[1024];
                    VIR_WARN("Failed to stat %s, most peculiar : %s",
                             binary, virStrerror(errno, ebuf, sizeof(ebuf)));
                    binary_mtime = 0;
                }

                if (old_caps && binary_mtime)
                    probe = !qemuCapsGetOldMachines("hvm", info->arch,
                                                    info->wordsize, kvmbin,
                                                    binary_mtime, old_caps,
                                                    &machines, &nmachines);
                if (probe &&
                    qemuCapsProbeMachineTypes(kvmbin, kvmbinCaps,
                                              &machines, &nmachines) < 0)
                    goto error;
            }

            if ((dom = virCapabilitiesAddGuestDomain(guest,
                                                     "kvm",
                                                     kvmbin ? kvmbin : binary,
                                                     NULL,
                                                     nmachines,
                                                     machines)) == NULL) {
                goto error;
            }

            machines = NULL;
            nmachines = 0;

            dom->info.emulator_mtime = binary_mtime;
        }
    } else {
        if (virCapabilitiesAddGuestDomain(guest,
                                          "kvm",
                                          NULL,
                                          NULL,
                                          0,
                                          NULL) == NULL)
            goto error;
    }

    if (info->nflags) {
        for (i = 0 ; i < info->nflags ; i++) {
            if (virCapabilitiesAddGuestFeature(guest,
                                               info->flags[i].name,
                                               info->flags[i].default_on,
                                               info->flags[i].toggle) == NULL)
                goto error;
        }
    }

    ret = 0;

cleanup:
    VIR_FREE(binary);
    VIR_FREE(kvmbin);
    virObjectUnref(qemubinCaps);
    virObjectUnref(kvmbinCaps);

    return ret;

no_memory:
    virReportOOMError();

error:
    virCapabilitiesFreeMachines(machines, nmachines);

    goto cleanup;
}


static int
qemuCapsInitCPU(virCapsPtr caps,
                 const char *arch)
{
    virCPUDefPtr cpu = NULL;
    union cpuData *data = NULL;
    virNodeInfo nodeinfo;
    int ret = -1;

    if (VIR_ALLOC(cpu) < 0
        || !(cpu->arch = strdup(arch))) {
        virReportOOMError();
        goto error;
    }

    if (nodeGetInfo(NULL, &nodeinfo))
        goto error;

    cpu->type = VIR_CPU_TYPE_HOST;
    cpu->sockets = nodeinfo.sockets;
    cpu->cores = nodeinfo.cores;
    cpu->threads = nodeinfo.threads;

    if (!(data = cpuNodeData(arch))
        || cpuDecode(cpu, data, NULL, 0, NULL) < 0)
        goto error;

    caps->host.cpu = cpu;

    ret = 0;

cleanup:
    cpuDataFree(arch, data);

    return ret;

error:
    virCPUDefFree(cpu);
    goto cleanup;
}


static int qemuDefaultConsoleType(const char *ostype ATTRIBUTE_UNUSED)
{
    return VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;
}


virCapsPtr qemuCapsInit(virCapsPtr old_caps)
{
    struct utsname utsname;
    virCapsPtr caps;
    int i;
    char *xenner = NULL;

    /* Really, this never fails - look at the man-page. */
    uname (&utsname);

    if ((caps = virCapabilitiesNew(utsname.machine,
                                   1, 1)) == NULL)
        goto no_memory;

    /* Using KVM's mac prefix for QEMU too */
    virCapabilitiesSetMacPrefix(caps, (unsigned char[]){ 0x52, 0x54, 0x00 });

    /* Some machines have problematic NUMA toplogy causing
     * unexpected failures. We don't want to break the QEMU
     * driver in this scenario, so log errors & carry on
     */
    if (nodeCapsInitNUMA(caps) < 0) {
        virCapabilitiesFreeNUMAInfo(caps);
        VIR_WARN("Failed to query host NUMA topology, disabling NUMA capabilities");
    }

    if (old_caps == NULL || old_caps->host.cpu == NULL) {
        if (qemuCapsInitCPU(caps, utsname.machine) < 0)
            VIR_WARN("Failed to get host CPU");
    }
    else {
        caps->host.cpu = old_caps->host.cpu;
        old_caps->host.cpu = NULL;
    }

    /* Add the power management features of the host */

    if (virNodeSuspendGetTargetMask(&caps->host.powerMgmt) < 0)
        VIR_WARN("Failed to get host power management capabilities");

    virCapabilitiesAddHostMigrateTransport(caps,
                                           "tcp");

    /* First the pure HVM guests */
    for (i = 0 ; i < ARRAY_CARDINALITY(arch_info_hvm) ; i++)
        if (qemuCapsInitGuest(caps, old_caps,
                              utsname.machine,
                              &arch_info_hvm[i], 1) < 0)
            goto no_memory;

    /* Then possibly the Xen paravirt guests (ie Xenner */
    xenner = virFindFileInPath("xenner");

    if (xenner != NULL && virFileIsExecutable(xenner) == 0 &&
        access("/dev/kvm", F_OK) == 0) {
        for (i = 0 ; i < ARRAY_CARDINALITY(arch_info_xen) ; i++)
            /* Allow Xen 32-on-32, 32-on-64 and 64-on-64 */
            if (STREQ(arch_info_xen[i].arch, utsname.machine) ||
                (STREQ(utsname.machine, "x86_64") &&
                 STREQ(arch_info_xen[i].arch, "i686"))) {
                if (qemuCapsInitGuest(caps, old_caps,
                                      utsname.machine,
                                      &arch_info_xen[i], 0) < 0)
                    goto no_memory;
            }
    }

    VIR_FREE(xenner);

    /* QEMU Requires an emulator in the XML */
    virCapabilitiesSetEmulatorRequired(caps);

    caps->defaultConsoleTargetType = qemuDefaultConsoleType;

    return caps;

 no_memory:
    VIR_FREE(xenner);
    virCapabilitiesFree(caps);
    return NULL;
}


static int
qemuCapsComputeCmdFlags(const char *help,
                        unsigned int version,
                        unsigned int is_kvm,
                        unsigned int kvm_version,
                        qemuCapsPtr caps,
                        bool check_yajl ATTRIBUTE_UNUSED)
{
    const char *p;
    const char *fsdev, *netdev;

    if (strstr(help, "-no-kqemu"))
        qemuCapsSet(caps, QEMU_CAPS_KQEMU);
    if (strstr(help, "-enable-kqemu"))
        qemuCapsSet(caps, QEMU_CAPS_ENABLE_KQEMU);
    if (strstr(help, "-no-kvm"))
        qemuCapsSet(caps, QEMU_CAPS_KVM);
    if (strstr(help, "-enable-kvm"))
        qemuCapsSet(caps, QEMU_CAPS_ENABLE_KVM);
    if (strstr(help, "-no-reboot"))
        qemuCapsSet(caps, QEMU_CAPS_NO_REBOOT);
    if (strstr(help, "-name")) {
        qemuCapsSet(caps, QEMU_CAPS_NAME);
        if (strstr(help, ",process="))
            qemuCapsSet(caps, QEMU_CAPS_NAME_PROCESS);
    }
    if (strstr(help, "-uuid"))
        qemuCapsSet(caps, QEMU_CAPS_UUID);
    if (strstr(help, "-xen-domid"))
        qemuCapsSet(caps, QEMU_CAPS_XEN_DOMID);
    else if (strstr(help, "-domid"))
        qemuCapsSet(caps, QEMU_CAPS_DOMID);
    if (strstr(help, "-drive")) {
        const char *cache = strstr(help, "cache=");

        qemuCapsSet(caps, QEMU_CAPS_DRIVE);
        if (cache && (p = strchr(cache, ']'))) {
            if (memmem(cache, p - cache, "on|off", sizeof("on|off") - 1) == NULL)
                qemuCapsSet(caps, QEMU_CAPS_DRIVE_CACHE_V2);
            if (memmem(cache, p - cache, "directsync", sizeof("directsync") - 1))
                qemuCapsSet(caps, QEMU_CAPS_DRIVE_CACHE_DIRECTSYNC);
            if (memmem(cache, p - cache, "unsafe", sizeof("unsafe") - 1))
                qemuCapsSet(caps, QEMU_CAPS_DRIVE_CACHE_UNSAFE);
        }
        if (strstr(help, "format="))
            qemuCapsSet(caps, QEMU_CAPS_DRIVE_FORMAT);
        if (strstr(help, "readonly="))
            qemuCapsSet(caps, QEMU_CAPS_DRIVE_READONLY);
        if (strstr(help, "aio=threads|native"))
            qemuCapsSet(caps, QEMU_CAPS_DRIVE_AIO);
        if (strstr(help, "copy-on-read=on|off"))
            qemuCapsSet(caps, QEMU_CAPS_DRIVE_COPY_ON_READ);
        if (strstr(help, "bps="))
            qemuCapsSet(caps, QEMU_CAPS_DRIVE_IOTUNE);
    }
    if ((p = strstr(help, "-vga")) && !strstr(help, "-std-vga")) {
        const char *nl = strstr(p, "\n");

        qemuCapsSet(caps, QEMU_CAPS_VGA);

        if (strstr(p, "|qxl"))
            qemuCapsSet(caps, QEMU_CAPS_VGA_QXL);
        if ((p = strstr(p, "|none")) && p < nl)
            qemuCapsSet(caps, QEMU_CAPS_VGA_NONE);
    }
    if (strstr(help, "-spice"))
        qemuCapsSet(caps, QEMU_CAPS_SPICE);
    if (strstr(help, "boot=on"))
        qemuCapsSet(caps, QEMU_CAPS_DRIVE_BOOT);
    if (strstr(help, "serial=s"))
        qemuCapsSet(caps, QEMU_CAPS_DRIVE_SERIAL);
    if (strstr(help, "-pcidevice"))
        qemuCapsSet(caps, QEMU_CAPS_PCIDEVICE);
    if (strstr(help, "-mem-path"))
        qemuCapsSet(caps, QEMU_CAPS_MEM_PATH);
    if (strstr(help, "-chardev")) {
        qemuCapsSet(caps, QEMU_CAPS_CHARDEV);
        if (strstr(help, "-chardev spicevmc"))
            qemuCapsSet(caps, QEMU_CAPS_CHARDEV_SPICEVMC);
    }
    if (strstr(help, "-balloon"))
        qemuCapsSet(caps, QEMU_CAPS_BALLOON);
    if (strstr(help, "-device")) {
        qemuCapsSet(caps, QEMU_CAPS_DEVICE);
        /*
         * When -device was introduced, qemu already supported drive's
         * readonly option but didn't advertise that.
         */
        qemuCapsSet(caps, QEMU_CAPS_DRIVE_READONLY);
    }
    if (strstr(help, "-nodefconfig"))
        qemuCapsSet(caps, QEMU_CAPS_NODEFCONFIG);
    if (strstr(help, "-no-user-config"))
        qemuCapsSet(caps, QEMU_CAPS_NO_USER_CONFIG);
    /* The trailing ' ' is important to avoid a bogus match */
    if (strstr(help, "-rtc "))
        qemuCapsSet(caps, QEMU_CAPS_RTC);
    /* to wit */
    if (strstr(help, "-rtc-td-hack"))
        qemuCapsSet(caps, QEMU_CAPS_RTC_TD_HACK);
    if (strstr(help, "-no-hpet"))
        qemuCapsSet(caps, QEMU_CAPS_NO_HPET);
    if (strstr(help, "-no-acpi"))
        qemuCapsSet(caps, QEMU_CAPS_NO_ACPI);
    if (strstr(help, "-no-kvm-pit-reinjection"))
        qemuCapsSet(caps, QEMU_CAPS_NO_KVM_PIT);
    if (strstr(help, "-tdf"))
        qemuCapsSet(caps, QEMU_CAPS_TDF);
    if (strstr(help, "-enable-nesting"))
        qemuCapsSet(caps, QEMU_CAPS_NESTING);
    if (strstr(help, ",menu=on"))
        qemuCapsSet(caps, QEMU_CAPS_BOOT_MENU);
    if ((fsdev = strstr(help, "-fsdev"))) {
        qemuCapsSet(caps, QEMU_CAPS_FSDEV);
        if (strstr(fsdev, "readonly"))
            qemuCapsSet(caps, QEMU_CAPS_FSDEV_READONLY);
        if (strstr(fsdev, "writeout"))
            qemuCapsSet(caps, QEMU_CAPS_FSDEV_WRITEOUT);
    }
    if (strstr(help, "-smbios type"))
        qemuCapsSet(caps, QEMU_CAPS_SMBIOS_TYPE);

    if ((netdev = strstr(help, "-netdev"))) {
        /* Disable -netdev on 0.12 since although it exists,
         * the corresponding netdev_add/remove monitor commands
         * do not, and we need them to be able to do hotplug.
         * But see below about RHEL build. */
        if (version >= 13000) {
            if (strstr(netdev, "bridge"))
                qemuCapsSet(caps, QEMU_CAPS_NETDEV_BRIDGE);
           qemuCapsSet(caps, QEMU_CAPS_NETDEV);
        }
    }

    if (strstr(help, "-sdl"))
        qemuCapsSet(caps, QEMU_CAPS_SDL);
    if (strstr(help, "cores=") &&
        strstr(help, "threads=") &&
        strstr(help, "sockets="))
        qemuCapsSet(caps, QEMU_CAPS_SMP_TOPOLOGY);

    if (version >= 9000)
        qemuCapsSet(caps, QEMU_CAPS_VNC_COLON);

    if (is_kvm && (version >= 10000 || kvm_version >= 74))
        qemuCapsSet(caps, QEMU_CAPS_VNET_HDR);

    if (strstr(help, ",vhost=")) {
        qemuCapsSet(caps, QEMU_CAPS_VHOST_NET);
    }

    /* Do not use -no-shutdown if qemu doesn't support it or SIGTERM handling
     * is most likely buggy when used with -no-shutdown (which applies for qemu
     * 0.14.* and 0.15.0)
     */
    if (strstr(help, "-no-shutdown") && (version < 14000 || version > 15000))
        qemuCapsSet(caps, QEMU_CAPS_NO_SHUTDOWN);

    /*
     * Handling of -incoming arg with varying features
     *  -incoming tcp    (kvm >= 79, qemu >= 0.10.0)
     *  -incoming exec   (kvm >= 80, qemu >= 0.10.0)
     *  -incoming unix   (qemu >= 0.12.0)
     *  -incoming fd     (qemu >= 0.12.0)
     *  -incoming stdio  (all earlier kvm)
     *
     * NB, there was a pre-kvm-79 'tcp' support, but it
     * was broken, because it blocked the monitor console
     * while waiting for data, so pretend it doesn't exist
     */
    if (version >= 10000) {
        qemuCapsSet(caps, QEMU_CAPS_MIGRATE_QEMU_TCP);
        qemuCapsSet(caps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
        if (version >= 12000) {
            qemuCapsSet(caps, QEMU_CAPS_MIGRATE_QEMU_UNIX);
            qemuCapsSet(caps, QEMU_CAPS_MIGRATE_QEMU_FD);
        }
    } else if (kvm_version >= 79) {
        qemuCapsSet(caps, QEMU_CAPS_MIGRATE_QEMU_TCP);
        if (kvm_version >= 80)
            qemuCapsSet(caps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
    } else if (kvm_version > 0) {
        qemuCapsSet(caps, QEMU_CAPS_MIGRATE_KVM_STDIO);
    }

    if (version >= 10000)
        qemuCapsSet(caps, QEMU_CAPS_0_10);

    if (version >= 11000)
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_BLK_SG_IO);

    /* While JSON mode was available in 0.12.0, it was too
     * incomplete to contemplate using. The 0.13.0 release
     * is good enough to use, even though it lacks one or
     * two features. This is also true of versions of qemu
     * built for RHEL, labeled 0.12.1, but with extra text
     * in the help output that mentions that features were
     * backported for libvirt. The benefits of JSON mode now
     * outweigh the downside.
     */
#if HAVE_YAJL
    if (version >= 13000) {
        qemuCapsSet(caps, QEMU_CAPS_MONITOR_JSON);
    } else if (version >= 12000 &&
               strstr(help, "libvirt")) {
        qemuCapsSet(caps, QEMU_CAPS_MONITOR_JSON);
        qemuCapsSet(caps, QEMU_CAPS_NETDEV);
    }
#else
    /* Starting with qemu 0.15 and newer, upstream qemu no longer
     * promises to keep the human interface stable, but requests that
     * we use QMP (the JSON interface) for everything.  If the user
     * forgot to include YAJL libraries when building their own
     * libvirt but is targetting a newer qemu, we are better off
     * telling them to recompile (the spec file includes the
     * dependency, so distros won't hit this).  This check is
     * also in configure.ac (see $with_yajl).  */
    if (version >= 15000 ||
        (version >= 12000 && strstr(help, "libvirt"))) {
        if (check_yajl) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu binary requires libvirt to be "
                             "compiled with yajl"));
            return -1;
        }
        qemuCapsSet(caps, QEMU_CAPS_NETDEV);
    }
#endif

    if (version >= 13000)
        qemuCapsSet(caps, QEMU_CAPS_PCI_MULTIFUNCTION);

    /* Although very new versions of qemu advertise the presence of
     * the rombar option in the output of "qemu -device pci-assign,?",
     * this advertisement was added to the code long after the option
     * itself. According to qemu developers, though, rombar is
     * available in all qemu binaries from release 0.12 onward.
     * Setting the capability this way makes it available in more
     * cases where it might be needed, and shouldn't cause any false
     * positives (in the case that it did, qemu would produce an error
     * log and refuse to start, so it would be immediately obvious).
     */
    if (version >= 12000)
        qemuCapsSet(caps, QEMU_CAPS_PCI_ROMBAR);

    if (version >= 11000)
        qemuCapsSet(caps, QEMU_CAPS_CPU_HOST);
    return 0;
}

/* We parse the output of 'qemu -help' to get the QEMU
 * version number. The first bit is easy, just parse
 * 'QEMU PC emulator version x.y.z'
 * or
 * 'QEMU emulator version x.y.z'.
 *
 * With qemu-kvm, however, that is followed by a string
 * in parenthesis as follows:
 *  - qemu-kvm-x.y.z in stable releases
 *  - kvm-XX for kvm versions up to kvm-85
 *  - qemu-kvm-devel-XX for kvm version kvm-86 and later
 *
 * For qemu-kvm versions before 0.10.z, we need to detect
 * the KVM version number for some features. With 0.10.z
 * and later, we just need the QEMU version number and
 * whether it is KVM QEMU or mainline QEMU.
 */
#define QEMU_VERSION_STR_1  "QEMU emulator version"
#define QEMU_VERSION_STR_2  "QEMU PC emulator version"
#define QEMU_KVM_VER_PREFIX "(qemu-kvm-"
#define KVM_VER_PREFIX      "(kvm-"

#define SKIP_BLANKS(p) do { while ((*(p) == ' ') || (*(p) == '\t')) (p)++; } while (0)

int qemuCapsParseHelpStr(const char *qemu,
                         const char *help,
                         qemuCapsPtr caps,
                         unsigned int *version,
                         unsigned int *is_kvm,
                         unsigned int *kvm_version,
                         bool check_yajl)
{
    unsigned major, minor, micro;
    const char *p = help;
    char *strflags;

    *version = *is_kvm = *kvm_version = 0;

    if (STRPREFIX(p, QEMU_VERSION_STR_1))
        p += strlen(QEMU_VERSION_STR_1);
    else if (STRPREFIX(p, QEMU_VERSION_STR_2))
        p += strlen(QEMU_VERSION_STR_2);
    else
        goto fail;

    SKIP_BLANKS(p);

    major = virParseNumber(&p);
    if (major == -1 || *p != '.')
        goto fail;

    ++p;

    minor = virParseNumber(&p);
    if (minor == -1)
        goto fail;

    if (*p != '.') {
        micro = 0;
    } else {
        ++p;
        micro = virParseNumber(&p);
        if (micro == -1)
            goto fail;
    }

    SKIP_BLANKS(p);

    if (STRPREFIX(p, QEMU_KVM_VER_PREFIX)) {
        *is_kvm = 1;
        p += strlen(QEMU_KVM_VER_PREFIX);
    } else if (STRPREFIX(p, KVM_VER_PREFIX)) {
        int ret;

        *is_kvm = 1;
        p += strlen(KVM_VER_PREFIX);

        ret = virParseNumber(&p);
        if (ret == -1)
            goto fail;

        *kvm_version = ret;
    }

    *version = (major * 1000 * 1000) + (minor * 1000) + micro;

    if (qemuCapsComputeCmdFlags(help, *version, *is_kvm, *kvm_version,
                                caps, check_yajl) < 0)
        goto cleanup;

    strflags = virBitmapString(caps->flags);
    VIR_DEBUG("Version %u.%u.%u, cooked version %u, flags %s",
              major, minor, micro, *version, NULLSTR(strflags));
    VIR_FREE(strflags);

    if (*kvm_version)
        VIR_DEBUG("KVM version %d detected", *kvm_version);
    else if (*is_kvm)
        VIR_DEBUG("qemu-kvm version %u.%u.%u detected", major, minor, micro);

    return 0;

fail:
    p = strchr(help, '\n');
    if (!p)
        p = strchr(help, '\0');

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("cannot parse %s version number in '%.*s'"),
                   qemu, (int) (p - help), help);

cleanup:
    return -1;
}

static int
qemuCapsExtractDeviceStr(const char *qemu,
                         qemuCapsPtr caps)
{
    char *output = NULL;
    virCommandPtr cmd;
    int ret = -1;

    /* Cram together all device-related queries into one invocation;
     * the output format makes it possible to distinguish what we
     * need.  With qemu 0.13.0 and later, unrecognized '-device
     * bogus,?' cause an error in isolation, but are silently ignored
     * in combination with '-device ?'.  Upstream qemu 0.12.x doesn't
     * understand '-device name,?', and always exits with status 1 for
     * the simpler '-device ?', so this function is really only useful
     * if -help includes "device driver,?".  */
    cmd = qemuCapsProbeCommand(qemu, caps);
    virCommandAddArgList(cmd,
                         "-device", "?",
                         "-device", "pci-assign,?",
                         "-device", "virtio-blk-pci,?",
                         "-device", "virtio-net-pci,?",
                         "-device", "scsi-disk,?",
                         "-device", "PIIX4_PM,?",
                         "-device", "usb-redir,?",
                         NULL);
    /* qemu -help goes to stdout, but qemu -device ? goes to stderr.  */
    virCommandSetErrorBuffer(cmd, &output);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    ret = qemuCapsParseDeviceStr(output, caps);

cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);
    return ret;
}


int
qemuCapsParseDeviceStr(const char *str, qemuCapsPtr caps)
{
    /* Which devices exist. */
    if (strstr(str, "name \"hda-duplex\""))
        qemuCapsSet(caps, QEMU_CAPS_HDA_DUPLEX);
    if (strstr(str, "name \"hda-micro\""))
        qemuCapsSet(caps, QEMU_CAPS_HDA_MICRO);
    if (strstr(str, "name \"ccid-card-emulated\""))
        qemuCapsSet(caps, QEMU_CAPS_CCID_EMULATED);
    if (strstr(str, "name \"ccid-card-passthru\""))
        qemuCapsSet(caps, QEMU_CAPS_CCID_PASSTHRU);

    if (strstr(str, "name \"piix3-usb-uhci\""))
        qemuCapsSet(caps, QEMU_CAPS_PIIX3_USB_UHCI);
    if (strstr(str, "name \"piix4-usb-uhci\""))
        qemuCapsSet(caps, QEMU_CAPS_PIIX4_USB_UHCI);
    if (strstr(str, "name \"usb-ehci\""))
        qemuCapsSet(caps, QEMU_CAPS_USB_EHCI);
    if (strstr(str, "name \"ich9-usb-ehci1\""))
        qemuCapsSet(caps, QEMU_CAPS_ICH9_USB_EHCI1);
    if (strstr(str, "name \"vt82c686b-usb-uhci\""))
        qemuCapsSet(caps, QEMU_CAPS_VT82C686B_USB_UHCI);
    if (strstr(str, "name \"pci-ohci\""))
        qemuCapsSet(caps, QEMU_CAPS_PCI_OHCI);
    if (strstr(str, "name \"nec-usb-xhci\""))
        qemuCapsSet(caps, QEMU_CAPS_NEC_USB_XHCI);
    if (strstr(str, "name \"usb-redir\""))
        qemuCapsSet(caps, QEMU_CAPS_USB_REDIR);
    if (strstr(str, "usb-redir.filter"))
        qemuCapsSet(caps, QEMU_CAPS_USB_REDIR_FILTER);
    if (strstr(str, "name \"usb-hub\""))
        qemuCapsSet(caps, QEMU_CAPS_USB_HUB);
    if (strstr(str, "name \"ich9-ahci\""))
        qemuCapsSet(caps, QEMU_CAPS_ICH9_AHCI);
    if (strstr(str, "name \"virtio-blk-s390\"") ||
        strstr(str, "name \"virtio-net-s390\"") ||
        strstr(str, "name \"virtio-serial-s390\""))
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_S390);

    if (strstr(str, "name \"lsi53c895a\""))
        qemuCapsSet(caps, QEMU_CAPS_SCSI_LSI);
    if (strstr(str, "name \"virtio-scsi-pci\""))
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_SCSI_PCI);

    /* Prefer -chardev spicevmc (detected earlier) over -device spicevmc */
    if (!qemuCapsGet(caps, QEMU_CAPS_CHARDEV_SPICEVMC) &&
        strstr(str, "name \"spicevmc\""))
        qemuCapsSet(caps, QEMU_CAPS_DEVICE_SPICEVMC);

    /* Features of given devices. */
    if (strstr(str, "pci-assign.configfd"))
        qemuCapsSet(caps, QEMU_CAPS_PCI_CONFIGFD);
    if (strstr(str, "virtio-blk-pci.multifunction"))
        qemuCapsSet(caps, QEMU_CAPS_PCI_MULTIFUNCTION);
    if (strstr(str, "virtio-blk-pci.bootindex")) {
        qemuCapsSet(caps, QEMU_CAPS_BOOTINDEX);
        if (strstr(str, "pci-assign.bootindex"))
            qemuCapsSet(caps, QEMU_CAPS_PCI_BOOTINDEX);
    }
    if (strstr(str, "virtio-net-pci.tx="))
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_TX_ALG);
    if (strstr(str, "name \"qxl-vga\""))
        qemuCapsSet(caps, QEMU_CAPS_DEVICE_QXL_VGA);
    if (strstr(str, "virtio-blk-pci.ioeventfd"))
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_IOEVENTFD);
    if (strstr(str, "name \"sga\""))
        qemuCapsSet(caps, QEMU_CAPS_SGA);
    if (strstr(str, "virtio-blk-pci.event_idx"))
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_BLK_EVENT_IDX);
    if (strstr(str, "virtio-net-pci.event_idx"))
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_NET_EVENT_IDX);
    if (strstr(str, "virtio-blk-pci.scsi"))
        qemuCapsSet(caps, QEMU_CAPS_VIRTIO_BLK_SCSI);
    if (strstr(str, "scsi-disk.channel"))
        qemuCapsSet(caps, QEMU_CAPS_SCSI_DISK_CHANNEL);
    if (strstr(str, "scsi-block"))
        qemuCapsSet(caps, QEMU_CAPS_SCSI_BLOCK);
    if (strstr(str, "scsi-cd"))
        qemuCapsSet(caps, QEMU_CAPS_SCSI_CD);
    if (strstr(str, "ide-cd"))
        qemuCapsSet(caps, QEMU_CAPS_IDE_CD);
    /*
     * the iolimit detection is not really straight forward:
     * in qemu this is a capability of the block layer, if
     * present any of -device scsi-disk, virtio-blk-*, ...
     * will offer to specify logical and physical block size
     * and other properties...
     */
    if (strstr(str, ".logical_block_size") &&
        strstr(str, ".physical_block_size"))
        qemuCapsSet(caps, QEMU_CAPS_BLOCKIO);
    if (strstr(str, "PIIX4_PM.disable_s3="))
        qemuCapsSet(caps, QEMU_CAPS_DISABLE_S3);
    if (strstr(str, "PIIX4_PM.disable_s4="))
        qemuCapsSet(caps, QEMU_CAPS_DISABLE_S4);

    return 0;
}

int qemuCapsExtractVersionInfo(const char *qemu,
                               const char *arch,
                               bool check_yajl,
                               unsigned int *retversion,
                               qemuCapsPtr *retcaps)
{
    int ret = -1;
    unsigned int version, is_kvm, kvm_version;
    qemuCapsPtr caps = NULL;
    char *help = NULL;
    virCommandPtr cmd;

    if (retcaps)
        *retcaps = NULL;
    if (retversion)
        *retversion = 0;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(qemu)) {
        virReportSystemError(errno, _("Cannot find QEMU binary %s"), qemu);
        return -1;
    }

    cmd = qemuCapsProbeCommand(qemu, NULL);
    virCommandAddArgList(cmd, "-help", NULL);
    virCommandSetOutputBuffer(cmd, &help);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (!(caps = qemuCapsNew()) ||
        qemuCapsParseHelpStr(qemu, help, caps,
                             &version, &is_kvm, &kvm_version,
                             check_yajl) == -1)
        goto cleanup;

    /* Currently only x86_64 and i686 support PCI-multibus. */
    if (STREQLEN(arch, "x86_64", 6) ||
        STREQLEN(arch, "i686", 4)) {
        qemuCapsSet(caps, QEMU_CAPS_PCI_MULTIBUS);
    }

    /* S390 and probably other archs do not support no-acpi -
       maybe the qemu option parsing should be re-thought. */
    if (STRPREFIX(arch, "s390"))
        qemuCapsClear(caps, QEMU_CAPS_NO_ACPI);

    /* qemuCapsExtractDeviceStr will only set additional caps if qemu
     * understands the 0.13.0+ notion of "-device driver,".  */
    if (qemuCapsGet(caps, QEMU_CAPS_DEVICE) &&
        strstr(help, "-device driver,?") &&
        qemuCapsExtractDeviceStr(qemu, caps) < 0)
        goto cleanup;

    if (retversion)
        *retversion = version;
    if (retcaps) {
        *retcaps = caps;
        caps = NULL;
    }

    ret = 0;

cleanup:
    VIR_FREE(help);
    virCommandFree(cmd);
    virObjectUnref(caps);

    return ret;
}

static void
uname_normalize (struct utsname *ut)
{
    uname(ut);

    /* Map i386, i486, i586 to i686.  */
    if (ut->machine[0] == 'i' &&
        ut->machine[1] != '\0' &&
        ut->machine[2] == '8' &&
        ut->machine[3] == '6' &&
        ut->machine[4] == '\0')
        ut->machine[1] = '6';
}

int qemuCapsExtractVersion(virCapsPtr caps,
                           unsigned int *version)
{
    const char *binary;
    struct stat sb;
    struct utsname ut;

    if (*version > 0)
        return 0;

    uname_normalize(&ut);
    if ((binary = virCapabilitiesDefaultGuestEmulator(caps,
                                                      "hvm",
                                                      ut.machine,
                                                      "qemu")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot find suitable emulator for %s"), ut.machine);
        return -1;
    }

    if (stat(binary, &sb) < 0) {
        virReportSystemError(errno,
                             _("Cannot find QEMU binary %s"), binary);
        return -1;
    }

    if (qemuCapsExtractVersionInfo(binary, ut.machine, false,
                                   version, NULL) < 0) {
        return -1;
    }

    return 0;
}




qemuCapsPtr
qemuCapsNew(void)
{
    qemuCapsPtr caps;

    if (qemuCapsInitialize() < 0)
        return NULL;

    if (!(caps = virObjectNew(qemuCapsClass)))
        return NULL;

    if (!(caps->flags = virBitmapAlloc(QEMU_CAPS_LAST)))
        goto no_memory;

    return caps;

no_memory:
    virReportOOMError();
    virObjectUnref(caps);
    return NULL;
}


qemuCapsPtr qemuCapsNewCopy(qemuCapsPtr caps)
{
    qemuCapsPtr ret = qemuCapsNew();
    size_t i;

    if (!ret)
        return NULL;

    virBitmapCopy(ret->flags, caps->flags);

    ret->version = caps->version;
    ret->kvmVersion = caps->kvmVersion;

    if (caps->arch &&
        !(ret->arch = strdup(caps->arch)))
        goto no_memory;

    if (VIR_ALLOC_N(ret->cpuDefinitions, caps->ncpuDefinitions) < 0)
        goto no_memory;
    ret->ncpuDefinitions = caps->ncpuDefinitions;
    for (i = 0 ; i < caps->ncpuDefinitions ; i++) {
        if (!(ret->cpuDefinitions[i] = strdup(caps->cpuDefinitions[i])))
            goto no_memory;
    }

    if (VIR_ALLOC_N(ret->machineTypes, caps->nmachineTypes) < 0)
        goto no_memory;
    if (VIR_ALLOC_N(ret->machineAliases, caps->nmachineTypes) < 0)
        goto no_memory;
    ret->nmachineTypes = caps->nmachineTypes;
    for (i = 0 ; i < caps->nmachineTypes ; i++) {
        if (!(ret->machineTypes[i] = strdup(caps->machineTypes[i])))
            goto no_memory;
        if (caps->machineAliases[i] &&
            !(ret->machineAliases[i] = strdup(caps->machineAliases[i])))
            goto no_memory;
    }

    return ret;

no_memory:
    virReportOOMError();
    virObjectUnref(ret);
    return NULL;
}


void qemuCapsDispose(void *obj)
{
    qemuCapsPtr caps = obj;
    size_t i;

    VIR_FREE(caps->arch);

    for (i = 0 ; i < caps->nmachineTypes ; i++) {
        VIR_FREE(caps->machineTypes[i]);
        VIR_FREE(caps->machineAliases[i]);
    }
    VIR_FREE(caps->machineTypes);
    VIR_FREE(caps->machineAliases);

    for (i = 0 ; i < caps->ncpuDefinitions ; i++) {
        VIR_FREE(caps->cpuDefinitions[i]);
    }
    VIR_FREE(caps->cpuDefinitions);

    virBitmapFree(caps->flags);
}

void
qemuCapsSet(qemuCapsPtr caps,
            enum qemuCapsFlags flag)
{
    ignore_value(virBitmapSetBit(caps->flags, flag));
}


void
qemuCapsSetList(qemuCapsPtr caps, ...)
{
    va_list list;
    int flag;

    va_start(list, caps);
    while ((flag = va_arg(list, int)) < QEMU_CAPS_LAST)
        ignore_value(virBitmapSetBit(caps->flags, flag));
    va_end(list);
}


void
qemuCapsClear(qemuCapsPtr caps,
              enum qemuCapsFlags flag)
{
    ignore_value(virBitmapClearBit(caps->flags, flag));
}


char *qemuCapsFlagsString(qemuCapsPtr caps)
{
    return virBitmapString(caps->flags);
}


bool
qemuCapsGet(qemuCapsPtr caps,
            enum qemuCapsFlags flag)
{
    bool b;

    if (!caps || virBitmapGetBit(caps->flags, flag, &b) < 0)
        return false;
    else
        return b;
}


const char *qemuCapsGetArch(qemuCapsPtr caps)
{
    return caps->arch;
}


unsigned int qemuCapsGetVersion(qemuCapsPtr caps)
{
    return caps->version;
}


unsigned int qemuCapsGetKVMVersion(qemuCapsPtr caps)
{
    return caps->kvmVersion;
}


size_t qemuCapsGetCPUDefinitions(qemuCapsPtr caps,
                                 char ***names)
{
    *names = caps->cpuDefinitions;
    return caps->ncpuDefinitions;
}


size_t qemuCapsGetMachineTypes(qemuCapsPtr caps,
                               char ***names)
{
    *names = caps->machineTypes;
    return caps->nmachineTypes;
}


const char *qemuCapsGetCanonicalMachine(qemuCapsPtr caps,
                                        const char *name)

{
    size_t i;

    for (i = 0 ; i < caps->nmachineTypes ; i++) {
        if (!caps->machineAliases[i])
            continue;
        if (STREQ(caps->machineAliases[i], name))
            return caps->machineTypes[i];
    }

    return name;
}
