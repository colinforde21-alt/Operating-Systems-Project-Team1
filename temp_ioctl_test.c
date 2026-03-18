#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "morse_ioctl.h"

int main() {
    int fd = open("/dev/chardev", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // read the default
    unsigned int unit;
    ioctl(fd, MORSE_GET_UNIT, &unit);
    printf("default unit: %u ms\n", unit);

    // set a new value
    unit = 1000;
    ioctl(fd, MORSE_SET_UNIT, &unit);

    // verify the read back
    ioctl(fd, MORSE_GET_UNIT, &unit);
    printf("unit is now: %u ms\n", unit);

    close(fd);
    return 0;
}