Explanation:

Basic kernel module that prints a message to the kernel logs upon during initalisation and during removal.

Prerequisites:

`sudo apt install build-essential raspberrypi-kernel-headers`

Instructions to run:

run `dmesg -w` in its own window.

`make`

`sudo insmod hello.ko` to insert the module.

`sudo rmmod hello` to remove the module.	

