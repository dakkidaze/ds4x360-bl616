/* Fetch a USB descriptor via usbfs (used to inspect the HID report descriptor
 * the firmware returns for interface 3). Usage: getdesc <devpath> <type> <index> [len] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: getdesc <dev> <type> <index> [len]\n"); return 2; }
    const char *dev = argv[1];
    unsigned type = strtoul(argv[2], NULL, 0);
    unsigned index = strtoul(argv[3], NULL, 0);
    unsigned len = (argc > 4) ? strtoul(argv[4], NULL, 0) : 512;

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    unsigned char *buf = malloc(len);
    memset(buf, 0xCC, len);
    struct usbdevfs_ctrltransfer c = {0};
    /* Interface-recipient for HID class descriptors (0x21/0x22),
     * device-recipient otherwise. */
    c.bRequestType = (type == 0x21 || type == 0x22) ? 0x81 : 0x80;
    c.bRequest = 6;              /* GET_DESCRIPTOR */
    c.wValue = (unsigned short)((type << 8) | index);
    c.wIndex = (type == 0x21 || type == 0x22) ? 3 : 0;
    c.wLength = (unsigned short)len;
    c.timeout = 5000;
    c.data = buf;

    int r = ioctl(fd, USBDEVFS_CONTROL, &c);
    if (r < 0) { perror("ioctl"); close(fd); return 1; }
    printf("returned %d bytes (requested %u)\n", r, len);
    for (int i = 0; i < r; i++) {
        printf("%02x ", buf[i]);
        if ((i % 16) == 15) printf("\n");
    }
    printf("\n");
    close(fd);
    return 0;
}
