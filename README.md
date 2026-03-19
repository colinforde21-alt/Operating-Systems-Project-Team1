## Explanation:


Kernel module that creates a character device which allows the user to write to an LED connected via GPIO pins on a raspberry pi. And read alphanumeric characters from inputted morse code via a button which is also connected to GPIO pins on a raspberry pi. This kernel module also creates a proc file where relevant statistics are written to.


## Prerequisites:


A raspberry pi whose GPIO physical pin 3 is connected to a button and physical pin 13 is connected to an LED.


`sudo apt install build-essential raspberrypi-kernel-headers`


## Instructions to run:


`make` to compile morse.c to morse.o and generate the target morse.ko file.


`sudo insmod morse.ko` to insert the module.


`sudo cat /dev/chardev` to trigger a read which blocks until data is added to the read buffer via the button.


`echo "hello" | sudo tee /dev/chardev` to write "hello" to the character devicd which displays this string in morse on the LED.


`sudo rmmod morse` to remove the module.   

