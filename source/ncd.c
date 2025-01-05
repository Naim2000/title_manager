#include <string.h>
#include <errno.h>
#include <ogc/ipc.h>

static const char* ncd_devpath = "/dev/net/ncd/manage";
static int ncd_fd = -1;
static int ncd_heap = -1;

int NCD_Init() {
    if (ncd_fd < 0) ncd_fd = IOS_Open(ncd_devpath, 0x0);
    if (ncd_fd < 0) return ncd_fd;

    if (ncd_heap < 0) ncd_heap = iosCreateHeap(0x50);
    if (ncd_heap < 0) return ncd_heap;

    return 0;
}

void NCD_Shutdown() {
    if (ncd_fd > 0) IOS_Close(ncd_fd);
    ncd_fd = -1;
}

int NCD_ReadConfig(void* out) {
    if ((long)out % 0x20) return -EINVAL;

    return IOS_IoctlvFormat(ncd_heap, ncd_fd, 0x5, ":dd", out, 7004, NULL, 0);
}

int NCD_WriteConfig(void* in) {
    if ((long)in % 0x20) return -EINVAL;

    return IOS_IoctlvFormat(ncd_heap, ncd_fd, 0x6, "d:d", in, 7004, NULL, 0);
}

int NCD_GetWirelessMacAddress(void* out) {
    unsigned char buf[6] __attribute__((__aligned__(0x20))) = {};

    int ret = IOS_IoctlvFormat(ncd_heap, ncd_fd, 0x8, ":dd", NULL, 0, buf, 6);
    memcpy(out, buf, sizeof(buf));
    return ret;
}
