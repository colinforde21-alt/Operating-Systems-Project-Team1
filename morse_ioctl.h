#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define MORSE_MAGIC 'M'
#define MORSE_SET_UNIT _IOW(MORSE_MAGIC, 1, unsigned int)
#define MORSE_GET_UNIT _IOR(MORSE_MAGIC, 2, unsigned int)