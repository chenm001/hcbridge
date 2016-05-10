
// Copyright (c) 2012 Nokia, Inc.
// Copyright (c) 2013-2014 Quanta Research Cambridge, Inc.

// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "portal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

static int trace_hardware;//=1;
void send_portal_null(struct PortalInternal *pint, volatile unsigned int *buffer, unsigned int hdr, int sendFd)
{
}
int recv_portal_null(struct PortalInternal *pint, volatile unsigned int *buffer, int len, int *recvfd)
{
    return 0;
}
int notfull_null(PortalInternal *pint, unsigned int v)
{
    return 0;
}
int busy_portal_null(struct PortalInternal *pint, unsigned int v, const char *str)
{
    return 0;
}
void enableint_portal_null(struct PortalInternal *pint, int val)
{
}
int event_null(struct PortalInternal *pint)
{
    return -1;
}
unsigned int read_portal_memory(PortalInternal *pint, volatile unsigned int **addr)
{
    unsigned int rc = **addr;
    *addr += 1;
    return rc;
}
void write_portal_memory(PortalInternal *pint, volatile unsigned int **addr, unsigned int v)
{
    **addr = v;
    *addr += 1;
}
void write_fd_portal_memory(PortalInternal *pint, volatile unsigned int **addr, unsigned int v)
{
    **addr = v;
    *addr += 1;
}
volatile unsigned int *mapchannel_req_generic(struct PortalInternal *pint, unsigned int v, unsigned int size)
{
    return pint->transport->mapchannelInd(pint, v);
}
volatile unsigned int *mapchannel_hardware(struct PortalInternal *pint, unsigned int v)
{
    return &pint->map_base[PORTAL_FIFO(v)];
}
int notfull_hardware(PortalInternal *pint, unsigned int v)
{
    volatile unsigned int *tempp = pint->transport->mapchannelInd(pint, v) + 1;
    return pint->transport->read(pint, &tempp);
}
int busy_hardware(struct PortalInternal *pint, unsigned int v, const char *str)
{
    int count = 50;
    while (!pint->transport->notFull(pint, v) && ((pint->busyType == BUSY_SPIN) || count-- > 0))
        ; /* busy wait a bit on 'fifo not full' */
    if (count <= 0) {
        if (0 && pint->busyType == BUSY_TIMEWAIT)
            while (!pint->transport->notFull(pint, v)) {
                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 10000;
                select(0, NULL, NULL, NULL, &timeout);
            }
        else {
            PORTAL_PRINTF("putFailed: %s\n", str);
            if (pint->busyType == BUSY_EXIT)
                exit(1);
            return 1;
        }
    }
    return 0;
}
void enableint_hardware(struct PortalInternal *pint, int val)
{
    volatile unsigned int *enp = &(pint->map_base[PORTAL_CTRL_INTERRUPT_ENABLE]);
    pint->transport->write(pint, &enp, val);
}
int event_hardware(struct PortalInternal *pint)
{
    // handle all messasges from this portal instance
    volatile unsigned int *map_base = pint->map_base;
    // sanity check, to see the status of interrupt source and enable
    unsigned int queue_status;
    volatile unsigned int *statp = &map_base[PORTAL_CTRL_IND_QUEUE_STATUS];
    volatile unsigned int *srcp = &map_base[PORTAL_CTRL_INTERRUPT_STATUS];
    volatile unsigned int *enp = &map_base[PORTAL_CTRL_INTERRUPT_ENABLE];
    while ((queue_status = pint->transport->read(pint, &statp))) {
        if(trace_hardware) {
            unsigned int int_src = pint->transport->read(pint, &srcp);
            unsigned int int_en  = pint->transport->read(pint, &enp);
            PORTAL_PRINTF( "%s: (fpga%d) about to receive messages int=%08x en=%08x qs=%08x handler %p parent %p\n", __FUNCTION__, pint->fpga_number, int_src, int_en, queue_status, pint->handler, pint->parent);
        }
        if (pint->handler)
            pint->handler(pint, queue_status-1, 0);
        else {
            unsigned int int_src = pint->transport->read(pint, &srcp);
            unsigned int int_en  = pint->transport->read(pint, &enp);
            PORTAL_PRINTF( "%s: (fpga%d) no handler receive int=%08x en=%08x qs=%08x handler %p parent %p\n", __FUNCTION__, pint->fpga_number, int_src, int_en, queue_status, pint->handler, pint->parent);
            exit(-1);
        }
    }
    return -1;
}
