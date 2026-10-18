/* Read the firmware debug log via HID GET_REPORT(Feature 0xB4).
 * Usage: hidlog <devpath> [iterations] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: hidlog <dev> [iters]\n"); return 2; }
    int iters = (argc > 2) ? atoi(argv[2]) : 200;

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    unsigned char buf[64];
    for (int i = 0; i < iters; i++) {
        memset(buf, 0, sizeof(buf));
        struct usbdevfs_ctrltransfer c = {0};
        c.bRequestType = 0xA1;         /* IN, class, interface */
        c.bRequest = 0x01;             /* GET_REPORT */
        c.wValue = (0x03 << 8) | 0xB4; /* Feature report 0xB4 */
        c.wIndex = 3;                  /* HID interface */
        c.wLength = sizeof(buf);
        c.timeout = 3000;
        c.data = buf;

        int r = ioctl(fd, USBDEVFS_CONTROL, &c);
        if (r < 0) { perror("ioctl"); break; }
        if (r <= 1) break;             /* only the report-id echo, nothing left */
        fwrite(buf + 1, 1, r - 1, stdout);
        fflush(stdout);
        usleep(2000);
    }
    close(fd);
    return 0;
}
