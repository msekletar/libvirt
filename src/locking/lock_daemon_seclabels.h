/*
 * lock_daemon_seclabels.h: Security label mgmt
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

#ifndef __VIR_LOCK_DAEMON_SECLABELS_H__
# define __VIR_LOCK_DAEMON_SECLABELS_H__

typedef struct _virSeclabelSpace virSeclabelSpace;
typedef virSeclabelSpace *virSeclabelSpacePtr;

void virSeclabelSpaceFree(virSeclabelSpacePtr space);
virSeclabelSpacePtr virSeclabelSpaceNew(void);

int virSeclabelSpaceRemember(virSeclabelSpacePtr space,
                             const char *path,
                             const char *model,
                             const char *label);
int virSeclabelSpaceRecall(virSeclabelSpacePtr space,
                           const char *path,
                           const char *model,
                           char **label);

virJSONValuePtr virSeclabelSpacePreExecRestart(virSeclabelSpacePtr space);
virSeclabelSpacePtr virSeclabelSpacePostExecRestart(virJSONValuePtr object);
#endif /* __VIR_LOCK_DAEMON_SECLABELS_H__ */
