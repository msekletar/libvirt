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

# include <stdbool.h>
# include <unistd.h>

# define IOHELPER_BUFSIZE 1024

typedef enum {
    IOHELPER_MESSAGE_DATA,
    IOHELPER_MESSAGE_HOLE,
} iohelperMessageType;

typedef struct _iohelperMessage iohelperMessage;
typedef iohelperMessage *iohelperMessagePtr;
struct _iohelperMessage {
    int type; /* enum iohelperMessageType */

    union {
        /* type == IOHELPER_MESSAGE_DATA */
        struct {
            size_t buflen; /* length of @buf */
            char buf[IOHELPER_BUFSIZE];
        } buf;

        /* type == IOHELPER_MESSAGE_HOLE */
        unsigned long long length;
    } data;
};

ssize_t iohelperRead(const char *fdName, int fd, size_t buflen,
                     iohelperMessagePtr *msg, bool formatted);
ssize_t iohelperWrite(const char *fdName, int fd,
                      iohelperMessagePtr msg, bool formatted);

void iohelperFree(iohelperMessagePtr msg);

#endif /* __VIR_IOHELPER_H__ */
