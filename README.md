Explanation:

Kernel module that creates a character device that prints messages to the kernel log every time its opened, closed, read
from or written to.

Prerequisites:

`sudo apt install build-essential raspberrypi-kernel-headers`

Instructions to run:

run `dmesg -w` in its own window.

`make` to compile hello.c to hello.o and generate the target hello.ko file.

`sudo insmod hello.ko` to insert the module.

`sudo cat /dev/chardev` to trigger a read message.

`echo "hello" | sudo tee /dev/chardev` to trigger a write message.

`sudo rmmod hello` to remove the module.	

