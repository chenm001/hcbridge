
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
#include "sock_utils.h"
#include <assert.h>

static int trace_socket; // = 1;

static unsigned int tag_counter;
#define MAX_SIMULATOR_TILES     4
typedef struct bsim_fpga_map_entry{
  uint32_t name;
  int offset;
  int valid;
} bsim_fpga_map_entry;
static bsim_fpga_map_entry bsim_fpga_map[MAX_SIMULATOR_TILES][MAX_SIMULATOR_PORTAL_ID];

static uint32_t read_bsim(uint32_t portal_index, long base, uint32_t offset)
{
    static PortalInternal p;
    p.fpga_number = portal_index;
    volatile unsigned int *ptemp = &((volatile unsigned int *)base)[offset];
    return transportBsim.read(&p, &ptemp);
}
static void initialize_bsim_map(void)
{
  long base_tile = 0;
  uint32_t tile_index = 0;
  uint32_t num_tiles = read_bsim(0, base_tile, PORTAL_CTRL_NUM_TILES);
  if (num_tiles >= MAX_SIMULATOR_TILES) {
      PORTAL_PRINTF("%s: Number of tiles %d exceeds max allowed %d\n", num_tiles, MAX_SIMULATOR_TILES);
      exit(-1);
  }
  do{
    uint32_t portal_index = 0;
    long base_ptr = base_tile;
    uint32_t num_portals = read_bsim(portal_index, base_ptr, PORTAL_CTRL_NUM_PORTALS);
    do{
      uint32_t id = read_bsim(portal_index, base_ptr, PORTAL_CTRL_PORTAL_ID);
      assert(num_portals == read_bsim(portal_index, base_ptr, PORTAL_CTRL_NUM_PORTALS));
      assert(num_tiles   == read_bsim(portal_index, base_ptr, PORTAL_CTRL_NUM_TILES));
      if (id >= MAX_SIMULATOR_PORTAL_ID) {
        PORTAL_PRINTF("%s: [%d] readid too large %d\n", __FUNCTION__, portal_index, id);
        break;
      }
      bsim_fpga_map[tile_index][portal_index].name = id;
      bsim_fpga_map[tile_index][portal_index].offset = portal_index;
      bsim_fpga_map[tile_index][portal_index].valid = 1;
      if (trace_socket)
        PORTAL_PRINTF("%s: bsim_fpga_map[%d/%d][%d/%d]=%d (%d)\n", __FUNCTION__, tile_index, num_tiles, portal_index, num_portals,
                      bsim_fpga_map[tile_index][portal_index].name, (portal_index+1==num_portals));
      portal_index++;
      base_ptr += PORTAL_BASE_OFFSET;
    } while (portal_index < num_portals && portal_index < 32);
    tile_index++;
    base_tile += TILE_BASE_OFFSET;
  } while (tile_index < num_tiles && tile_index < MAX_SIMULATOR_TILES);
}

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>      // FIONBIO
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <semaphore.h>
#include <pthread.h>
#include <netdb.h>

void memdump(unsigned char *p, int len, const char *title)
{
int i;

    i = 0;
    while (len > 0) {
        if (!(i & 0xf)) {
            if (i > 0)
                fprintf(stderr, "\n");
            fprintf(stderr, "%s: ",title);
        }
        fprintf(stderr, "%02x ", *p++);
        i++;
        len--;
    }
    fprintf(stderr, "\n");
}

static pthread_mutex_t socket_mutex;
int global_sockfd = -1;

void connect_to_bsim(void)
{
  if (global_sockfd != -1)
    return;
  global_sockfd = init_connecting(SOCKET_NAME, NULL);
  pthread_mutex_init(&socket_mutex, NULL);
  initialize_bsim_map();
}

/*
 * BOARD_bluesim
 */
static struct memresponse shared_response;
static int shared_response_valid;
static uint32_t interrupt_value;
int poll_response(uint32_t id)
{
  int recvFd;
  if (!shared_response_valid) {
      if (portalRecvFd(global_sockfd, &shared_response, sizeof(shared_response), &recvFd) == sizeof(shared_response)) {
          if (shared_response.portal == MAGIC_PORTAL_FOR_SENDING_INTERRUPT)
              interrupt_value = shared_response.data;
          else
              shared_response_valid = 1;
      }
  }
  return shared_response_valid && shared_response.portal == id;
}
unsigned int bsim_poll_interrupt(void)
{
  if (global_sockfd == -1)
      return 0;
  pthread_mutex_lock(&socket_mutex);
  poll_response(-1);
  pthread_mutex_unlock(&socket_mutex);
  return interrupt_value;
}
/* functions called by READL() and WRITEL() macros in application software */
static unsigned int read_portal_bsim(PortalInternal *pint, volatile unsigned int **addr)
{
  struct memrequest foo = {pint->fpga_number, 0,*addr,0};

  pthread_mutex_lock(&socket_mutex);
  foo.data_or_tag = tag_counter++;
  portalSendFd(global_sockfd, &foo, sizeof(foo), -1);
  while (!poll_response(pint->fpga_number)) {
      struct timeval tv = {};
      tv.tv_usec = 10000;
      select(0, NULL, NULL, NULL, &tv);
  }
  unsigned int rc = shared_response.data;
  shared_response_valid = 0;
  pthread_mutex_unlock(&socket_mutex);
  return rc;
}

static void write_portal_bsim(PortalInternal *pint, volatile unsigned int **addr, unsigned int v)
{
  struct memrequest foo = {pint->fpga_number, 1,*addr,v};

  portalSendFd(global_sockfd, &foo, sizeof(foo), -1);
}
static void write_fd_portal_bsim(PortalInternal *pint, volatile unsigned int **addr, unsigned int v)
{
  struct memrequest foo = {pint->fpga_number, 1,*addr,v};

  portalSendFd(global_sockfd, &foo, sizeof(foo), v);
}

static int init_bsim(struct PortalInternal *pint, void *param)
{
    int found = 0;
    int i;
    initPortalHardware();
    connect_to_bsim();
    assert(pint->fpga_number < MAX_SIMULATOR_PORTAL_ID);
    struct bsim_fpga_map_entry* entry = bsim_fpga_map[pint->fpga_tile];
    for (i = 0; entry[i].valid; i++)
      if (entry[i].name == pint->fpga_number) {
        found = 1;
        pint->fpga_number = entry[i].offset;
        break;
      }
    if (!found) {
      PORTAL_PRINTF( "Error: init_bsim: did not find fpga_number %d in tile %d\n", pint->fpga_number, pint->fpga_tile);
      PORTAL_PRINTF( "    Found fpga numbers:");
      for (i = 0; entry[i].valid; i++)
        PORTAL_PRINTF( " %d", entry[i].name);
      PORTAL_PRINTF( "\n");
    }
    assert(found);
    pint->map_base = (volatile unsigned int*)(long)((pint->fpga_tile * TILE_BASE_OFFSET)+(pint->fpga_number * PORTAL_BASE_OFFSET));
    pint->transport->enableint(pint, 1);

    return 0;
}
int event_portal_bsim(struct PortalInternal *pint)
{
    if (pint->fpga_fd == -1 && !bsim_poll_interrupt())
        return -1;

    return event_hardware(pint);
}
PortalTransportFunctions transportBsim = {
    init_bsim, read_portal_bsim, write_portal_bsim, write_fd_portal_bsim, mapchannel_hardware, mapchannel_req_generic,
    send_portal_null, recv_portal_null, busy_hardware, enableint_hardware, event_portal_bsim, notfull_hardware};
