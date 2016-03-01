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
#include "sock_utils.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h> // ctime
#include <stdarg.h> // for portal_printf
#include <sys/wait.h>
#include <sys/stat.h>
#include <libgen.h>  // dirname
#include <pthread.h>

int simulator_dump_vcd = 0;
const char *simulator_vcd_name = "dump.vcd";
// set this to 1 to suppress call to fpgajtag
int noprogram = 0;

static int trace_portal;//= 1;

int global_pa_fd = -1;
PortalInternal *utility_portal = 0x0;

/*
 * Initialize control data structure for portal
 */
void init_portal_internal(PortalInternal *pint, int id, int tile,
    PORTAL_INDFUNC handler, void *cb, PortalTransportFunctions *item, void *param, void *parent,
    uint32_t reqinfo)
{
    int rc;
    memset(pint, 0, sizeof(*pint));
    if(!utility_portal)
      utility_portal = pint;
    pint->fpga_number = id;
    pint->fpga_tile = tile;
    pint->fpga_fd = -1;
    pint->muxid = -1;
    pint->handler = handler;
    pint->cb = (PortalHandlerTemplate *)cb;
    pint->parent = parent;
    pint->reqinfo = reqinfo;
    if(trace_portal)
        PORTAL_PRINTF("%s: **initialize portal_%d_%d handler %p cb %p parent %p\n", __FUNCTION__, pint->fpga_tile, pint->fpga_number, handler, cb, parent);
    if (!item) {
        // Use defaults for transport handling methods
#ifdef BOARD_bluesim
        item = &transportBsim;
#else
    #error Unsupport mode
#endif
    }
    pint->item = item;
    rc = pint->item->init(pint, param);
    if (rc != 0) {
        PORTAL_PRINTF("%s: failed to initialize Portal portal_%d_%d\n", __FUNCTION__, pint->fpga_tile, pint->fpga_number);
        exit(1);
    }
}
int portal_disconnect(struct PortalInternal *pint)
{
    if(trace_portal)
        PORTAL_PRINTF("[%s:%d] fpgafd %d num %d cli %d\n", __FUNCTION__, __LINE__, pint->fpga_fd, pint->client_fd_number, pint->client_fd[0], pint->client_fd[1]);
    close(pint->fpga_fd);
    if (pint->client_fd_number > 0)
        close(pint->client_fd[--pint->client_fd_number]);
    return 0;
}

char *getExecutionFilename(char *buf, int buflen)
{
    int rc, fd;
    char *filename = 0;
    buf[0] = 0;
    fd = open("/proc/self/maps", O_RDONLY);
    while ((rc = read(fd, buf, buflen-1)) > 0) {
    buf[rc] = 0;
    rc = 0;
    while(buf[rc]) {
        char *endptr;
        unsigned long addr = strtoul(&buf[rc], &endptr, 16);
        if (endptr && *endptr == '-') {
        char *endptr2;
        unsigned long addr2 = strtoul(endptr+1, &endptr2, 16);
        if (addr <= (unsigned long)&initPortalHardware && (unsigned long)&initPortalHardware <= addr2) {
            filename = strstr(endptr2, "  ");
            while (*filename == ' ')
            filename++;
            endptr2 = strstr(filename, "\n");
            if (endptr2)
            *endptr2 = 0;
            fprintf(stderr, "buffer %s\n", filename);
            goto endloop;
        }
        }
        while(buf[rc] && buf[rc] != '\n')
        rc++;
        if (buf[rc])
        rc++;
    }
    }
endloop:
    if (!filename) {
    fprintf(stderr, "[%s:%d] could not find execution filename\n", __FUNCTION__, __LINE__);
    return 0;
    }
    return filename;
}
/*
 * One time initialization of portal framework
 */
static pthread_once_t once_control;
static void initPortalHardwareOnce(void)
{
    /*
     * fork/exec 'fpgajtag' to download bits to hardware
     * (the FPGA bits are stored as an extra ELF segment in the executable file)
     */
    int pid = fork();
    if (pid == -1) {
    fprintf(stderr, "[%s:%d] fork error\n", __FUNCTION__, __LINE__);
        exit(-1);
    }
    else if (pid) {
        //checkSignature("/dev/portalmem", PA_SIGNATURE);
    }
    else {
#define MAX_PATH 2000
        static char buf[400000];
        char *filename = NULL;
        char *argv[] = { (char *)"fpgajtag", NULL, NULL, NULL, NULL, NULL, NULL, NULL};
        int ind = 1;
        if (noprogram || getenv("NOFPGAJTAG") || getenv("NOPROGRAM"))
            exit(0);
#ifndef SIMULATOR_USE_PATH
    filename = getExecutionFilename(buf, sizeof(buf));
#endif

    char *bindir = (filename) ? dirname(filename) : 0;
    static char exename[MAX_PATH];
    char *library_path = 0;
    if (getenv("DUMP_VCD")) {
      simulator_dump_vcd = 1;
      simulator_vcd_name = getenv("DUMP_VCD");
    }
#if defined(BOARD_bluesim)
    const char *exetype = "bsim";
    if (simulator_dump_vcd) {
      argv[ind++] = (char*)"-V";
      argv[ind++] = (char*)simulator_vcd_name;
    }
#endif
    if (bindir)
        sprintf(exename, "%s/%s", bindir, exetype);
    else
        sprintf(exename, "%s", exetype);
    argv[0] = exename;
    if (trace_portal) fprintf(stderr, "[%s:%d] %s %s *******\n", __FUNCTION__, __LINE__, exetype, exename);
        argv[ind++] = NULL;
    if (bindir) {
        const char *old_library_path = getenv("LD_LIBRARY_PATH");
        int library_path_len = strlen(bindir);
        if (old_library_path)
        library_path_len += strlen(old_library_path);
        library_path = (char *)malloc(library_path_len + 2);
        if (old_library_path)
        snprintf(library_path, library_path_len+2, "%s:%s", bindir, old_library_path);
        else
        snprintf(library_path, library_path_len+1, "%s", bindir);
        setenv("LD_LIBRARY_PATH", library_path, 1);
        if (trace_portal) fprintf(stderr, "[%s:%d] LD_LIBRARY_PATH %s *******\n", __FUNCTION__, __LINE__, library_path);
    }
        execvp (exename, argv);
        fprintf(stderr, "[%s:%d] exec(%s) failed errno=%d:%s\n", __FUNCTION__, __LINE__, exename, errno, strerror(errno));
        exit(-1);
    }
}
void initPortalHardware(void)
{
    pthread_once(&once_control, initPortalHardwareOnce);
}

/*
 * Miscellaneous utility functions
 */
int setClockFrequency(int clkNum, long requestedFrequency, long *actualFrequency)
{
    int status = -1;
    initPortalHardware();
    return status;
}
