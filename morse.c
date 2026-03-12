#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/gpio.h>

#define DEV_NAME "chardev"
#define SIZE 256
#define LED_PIN 529

typedef struct {
    char character;
    const char *morse;
} MorseEntry;

static MorseEntry morseDict[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},
    {'D', "-.."},   {'E', "."},     {'F', "..-."},
    {'G', "--."},   {'H', "...."},  {'I', ".."},
    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
    {'M', "--"},    {'N', "-."},    {'O', "---"},
    {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},
    {'S', "..."},   {'T', "-"},     {'U', "..-"},
    {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
    {'Y', "-.--"},  {'Z', "--.."},
	
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"},
    {'3', "...--"}, {'4', "....-"}, {'5', "....."},
    {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},

    {' ', "/"}
};

static const char* getMorse(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;

    int size = sizeof(morseDict) / sizeof(morseDict[0]);
    for (int i = 0; i < size; i++) {
        if (morseDict[i].character == c)
            return morseDict[i].morse;
    }
    return NULL;
}

static char buffer[SIZE];
static int buf_len = 0;
static int buf_read = 0;

static DEFINE_MUTEX(buffer_mutex);

static DECLARE_WAIT_QUEUE_HEAD(hello_read_queue);
static DECLARE_WAIT_QUEUE_HEAD(hello_write_queue);
static ssize_t hello_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	int bytes_read = 0;

	if((wait_event_interruptible(hello_read_queue, buf_read < buf_len)) != 0)
		return -ERESTARTSYS;

	if (mutex_lock_interruptible(&buffer_mutex) < 0)
		return -ERESTARTSYS;

	while(len && buf_read < buf_len) {
		put_user(buffer[buf_read++], buf++);
		--len;
		++bytes_read;
	}

	mutex_unlock(&buffer_mutex);

	wake_up_interruptible(&hello_write_queue);

	return (ssize_t) bytes_read;
}
static ssize_t hello_write(struct file *filp, const char __user *buf, size_t length, loff_t *off)
{

	int bytes_written = 0;

	if (wait_event_interruptible(hello_write_queue, buf_len < SIZE) < 0)
		return -ERESTARTSYS;

	if (mutex_lock_interruptible(&buffer_mutex) < 0)
		return -ERESTARTSYS;

	while (length && buf_len < SIZE - 1) {
		get_user(buffer[buf_len++], buf++);
		bytes_written++;
		length--;
	}

	buffer[buf_len] = '\0';

	mutex_unlock(&buffer_mutex);

	wake_up_interruptible(&hello_read_queue);

	return (ssize_t) bytes_written;
}
static int hello_open(struct inode *inode, struct file *file)
{
	gpio_set_value(LED_PIN, 1);
	pr_info("Opening file!\n");
	return 0;
}
static int hello_release(struct inode *inode, struct file *file)
{
	gpio_set_value(LED_PIN, 0);
	pr_info("Closing file!\n");
	return 0;
}
static struct file_operations fops = {
	.open = hello_open,
	.read = hello_read,
	.write = hello_write,
	.release = hello_release,
};

static dev_t dev;
static struct cdev hello_cdev;
static struct class *hello_class;
static struct device *hello_device;

static int __init hello_init(void)
{
	if (gpio_request(LED_PIN, "led") < 0)
		return -1;
	gpio_direction_output(LED_PIN, 0);

	if ((alloc_chrdev_region(&dev, 0, 1, DEV_NAME)) < 0) {
		pr_alert("Cannot allocate major number!\n");
		gpio_free(LED_PIN);
		return -1;
	}

	cdev_init(&hello_cdev, &fops);

	if ((cdev_add(&hello_cdev, dev, 1)) < 0) {
		pr_alert("Cannot add device!\n");
		unregister_chrdev_region(dev, 1);
		gpio_free(LED_PIN);
		return -1;
	}

	hello_class = class_create(DEV_NAME);
	if (IS_ERR(hello_class)){
		pr_alert("Creating class failed!\n");
		cdev_del(&hello_cdev);
		unregister_chrdev_region(dev, 1);
		gpio_free(LED_PIN);
		return -1;
	}

	hello_device = device_create(hello_class, NULL, dev, NULL, DEV_NAME);
	if (IS_ERR(hello_device)){
		pr_alert("Creating device failed!\n");
		cdev_del(&hello_cdev);
		class_destroy(hello_class);
		unregister_chrdev_region(dev, 1);
		gpio_free(LED_PIN);
		return -1;
	}


	return 0;
}

static void __exit hello_exit(void)
{
	device_destroy(hello_class, dev);
	class_destroy(hello_class);
	cdev_del(&hello_cdev);
	unregister_chrdev_region(dev, 1);
	gpio_free(LED_PIN);
	pr_info("Module unloaded succesfully!");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
