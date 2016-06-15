/*
 * iohelper_message.h: Formatted messages between iohelper and us
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


#ifndef __VIR_IOHELPER_MESSAGE_H__
# define __VIR_IOHELPER_MESSAGE_H__

typedef struct _iohelperCtl iohelperCtl;
typedef iohelperCtl *iohelperCtlPtr;

iohelperCtlPtr iohelperCtlNew(int fd);

void iohelperCtlFree(iohelperCtlPtr ctl);

int iohelperCtlGetFD(iohelperCtlPtr ctl);

ssize_t iohelperReadBuf(iohelperCtlPtr ctl,
                        char *buf,
                        size_t len);
ssize_t
iohelperWriteBuf(iohelperCtlPtr ctl,
                 const char *buf,
                 size_t len);

#endif /* __VIR_IOHELPER_H__ */
