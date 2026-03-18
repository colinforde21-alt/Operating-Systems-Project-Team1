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
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/ioctl.h>
#include <linux/atomic.h>

#define DEV_NAME "chardev"
#define SIZE 256
#define LED_PIN 529
#define BTN_PIN 514

#define MORSE_MAGIC 'M'
#define MORSE_SET_UNIT _IOW(MORSE_MAGIC, 1, unsigned int)
#define MORSE_GET_UNIT _IOR(MORSE_MAGIC, 2, unsigned int)

typedef struct {
    char character;
    const char *morse;
} MorseEntry;

static const char *letters[] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....",
    "..",   ".---", "-.-",  ".-..", "--",   "-.",   "---",  ".--.",
    "--.-", ".-.",  "...",  "-",    "..-",  "...-", ".--",  "-..-",
    "-.--", "--.."
};

static const char *digits[] = {
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----."
};

static const char* getMorse(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == ' ')              return "/";
    return NULL;
}

static atomic_t morse_unit_ms = ATOMIC_INIT(200); 

static unsigned int get_dot(void)    { 
	return atomic_read(&morse_unit_ms); 
}
static unsigned int get_dash(void)   { 
	return atomic_read(&morse_unit_ms) * 3; 
}
static unsigned int get_sym_gap(void){ 
	return atomic_read(&morse_unit_ms); 
}
static unsigned int get_let_gap(void){ 
	return atomic_read(&morse_unit_ms) * 3; 
}
static unsigned int get_word_gap(void){ 
	return atomic_read(&morse_unit_ms) * 7; 
}

static char led_buffer[SIZE];
static int led_head = 0;
static int led_tail = 0;

static struct task_struct *led_thread;

static DEFINE_MUTEX(led_buffer_mutex);
static DECLARE_WAIT_QUEUE_HEAD(hello_led_queue);


static char morse_buffer[SIZE];
static int morse_head = 0;
static int morse_tail = 0;

static struct task_struct *morse_thread;

static DEFINE_MUTEX(morse_buffer_mutex);
static DECLARE_WAIT_QUEUE_HEAD(hello_morse_queue);


static int morse_buf_empty(void)
{
	return READ_ONCE(morse_head) == READ_ONCE(morse_tail);
}

static int morse_buf_full(void)
{
	return ((READ_ONCE(morse_tail) + 1) % SIZE) == READ_ONCE(morse_head);
}

static int led_buf_empty(void)
{
	return READ_ONCE(led_head) == READ_ONCE(led_tail);
}

static int led_buf_full(void)
{
	return ((READ_ONCE(led_tail) + 1) % SIZE) == READ_ONCE(led_head);
}

static int led_write_thread(void *pv)
{
	while (!kthread_should_stop()) {

		if(wait_event_interruptible(hello_led_queue, !led_buf_empty()) < 0)
			continue;
		if(mutex_lock_interruptible(&led_buffer_mutex) < 0)
			continue;
		if(led_buf_empty()) {
			mutex_unlock(&led_buffer_mutex);
			continue;
		}

		char c = led_buffer[led_head];
		led_head = (led_head + 1) % SIZE;

		mutex_unlock(&led_buffer_mutex);

		switch(c) {
			case '.':
				gpio_set_value(LED_PIN, 1);
				msleep(get_dot());
				gpio_set_value(LED_PIN, 0);
				msleep(get_sym_gap());
				break;
			case '-':
				gpio_set_value(LED_PIN, 1);
				msleep(get_dash());
				gpio_set_value(LED_PIN, 0);
				msleep(get_sym_gap());
				break;
			case '/':
				msleep(get_word_gap());
				break;
			case ' ':
				msleep(get_let_gap());
				break;
		}

		wake_up_interruptible(&hello_led_queue);
	}
	return 0;
}


static bool morse_buffer_put_char(char c)
{
    if (morse_buf_full())
        return false;

    morse_buffer[morse_tail] = c;
    morse_tail = (morse_tail + 1) % SIZE;
    return true;
}

static bool morse_buffer_put_str(const char *s)
{
    while (*s) {
        if (!morse_buffer_put_char(*s++))
            return false;
    }
    return true;
}

static int button_polling_thread(void *pv)
{
    int last_sample = gpio_get_value(BTN_PIN);
    int stable_state = last_sample;
    int count = 1;
    ktime_t press_time;

    while (!kthread_should_stop()) {
        int sample = gpio_get_value(BTN_PIN);
		s64 threshold = (s64)atomic_read(&morse_unit_ms) * 2;

        if (sample == last_sample) {
            count++;
        } else {
            last_sample = sample;
            count = 1;
        }

        if (count >= 5 && stable_state != last_sample) {
            stable_state = last_sample;

            if (stable_state == 0) {
                press_time = ktime_get();
            } else {
                s64 elapsed_ms = ktime_ms_delta(ktime_get(), press_time);

                if (mutex_lock_interruptible(&morse_buffer_mutex) < 0)
                    continue;

                if (elapsed_ms >= threshold) {
                    morse_buffer_put_str("-");
                } else if (elapsed_ms > 0) {
                    morse_buffer_put_str(".");
		} else {
			continue;
		}

                mutex_unlock(&morse_buffer_mutex);
                wake_up_interruptible(&hello_morse_queue);
            }
        }

        msleep(2);
    }

    return 0;
}



static ssize_t hello_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	int bytes_read = 0;

	if((wait_event_interruptible(hello_morse_queue, !morse_buf_empty()) != 0))
		return -ERESTARTSYS;

	if (mutex_lock_interruptible(&morse_buffer_mutex) < 0)
		return -ERESTARTSYS;

	while(len && !morse_buf_empty()) {
		if (put_user(morse_buffer[morse_head], buf++)) {
			mutex_unlock(&morse_buffer_mutex);
			return -EFAULT;
		}

		morse_head = (morse_head + 1) % SIZE;
		--len;
		++bytes_read;
	}

	mutex_unlock(&morse_buffer_mutex);

	wake_up_interruptible(&hello_morse_queue);

	return (ssize_t) bytes_read;
}


static bool led_buffer_put_char(char c)
{
    if (led_buf_full())
        return false;

    led_buffer[led_tail] = c;
    led_tail = (led_tail + 1) % SIZE;
    return true;
}

static bool led_buffer_put_str(const char *s)
{
    while (*s) {
        if (!led_buffer_put_char(*s++))
            return false;
    }
    return true;
}
static int led_buf_avail(void)
{
    if (led_tail >= led_head)
        return SIZE - (led_tail - led_head) - 1;
    return led_head - led_tail - 1;
}
static ssize_t hello_write(struct file *filp, const char __user *buf, size_t length, loff_t *off)
{
    int bytes_written = 0;
    char c;



    if (wait_event_interruptible(hello_led_queue, !led_buf_full()) < 0)
        return -ERESTARTSYS;

    if (mutex_lock_interruptible(&led_buffer_mutex) < 0)
        return -ERESTARTSYS;

    while (length) {
        if (get_user(c, buf++)) {
		mutex_unlock(&led_buffer_mutex);
		return -EFAULT;
	}

        const char *morse = getMorse(c);
        if (!morse) {
            bytes_written++;
            length--;
            continue;
        }

        int morse_len = strlen(morse);
        int needed = morse_len + 1; // +1 for the separator space

        if (led_buf_avail() < needed)
            break;

        led_buffer_put_str(morse);
        led_buffer_put_char(' ');

        bytes_written++;
        length--;
    }

    mutex_unlock(&led_buffer_mutex);

    wake_up_interruptible(&hello_led_queue);

    return (ssize_t) bytes_written;
}

static int hello_open(struct inode *inode, struct file *file)
{
	pr_info("Opening file!\n");
	return 0;
}
static int hello_release(struct inode *inode, struct file *file)
{
	pr_info("Closing file!\n");
	return 0;
}
static struct file_operations fops = {
	.open = hello_open,
	.read = hello_read,
	.write = hello_write,
	.release = hello_release,
};

static long hello_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    unsigned int unit;

    switch (cmd) {
    case MORSE_SET_UNIT:
        if (copy_from_user(&unit, (unsigned int __user *)arg, sizeof(unit)))
            return -EFAULT;
        if (unit < 50 || unit > 2000)
            return -EINVAL;
        atomic_set(&morse_unit_ms, unit);
        return 0;

    case MORSE_GET_UNIT:
        unit = atomic_read(&morse_unit_ms);
        if (copy_to_user((unsigned int __user *)arg, &unit, sizeof(unit)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

static dev_t dev;
static struct cdev hello_cdev;
static struct class *hello_class;
static struct device *hello_device;

static int __init hello_init(void)
{
	if (gpio_request(LED_PIN, "led") < 0)
		return -1;
	gpio_direction_output(LED_PIN, 0);

	if (gpio_request(BTN_PIN, "btn") < 0) {
		gpio_free(LED_PIN);
		return -1;
	}
	gpio_direction_input(BTN_PIN);

	if ((alloc_chrdev_region(&dev, 0, 1, DEV_NAME)) < 0) {
		pr_alert("Cannot allocate major number!\n");
		goto r_region;
	}

	cdev_init(&hello_cdev, &fops);

	if ((cdev_add(&hello_cdev, dev, 1)) < 0) {
		pr_alert("Cannot add device!\n");
		goto r_cdev;
	}

	hello_class = class_create(DEV_NAME);
	if (IS_ERR(hello_class)){
		pr_alert("Creating class failed!\n");
		goto r_class;
	}

	hello_device = device_create(hello_class, NULL, dev, NULL, DEV_NAME);
	if (IS_ERR(hello_device)){
		pr_alert("Creating device failed!\n");
		goto r_device;
	}

	led_thread = kthread_run(led_write_thread, NULL, "LED Thread");
	if (IS_ERR(led_thread)) {
		pr_err("Cannot create thread!");
		goto r_thread;
	}

	morse_thread = kthread_run(button_polling_thread, NULL, "Button Poling Thread");
	if (IS_ERR(morse_thread)) {
		pr_err("Cannot create thread!");
		kthread_stop(led_thread);
		goto r_thread;
	}
	return 0;

r_thread:
	device_destroy(hello_class, dev);
r_device:
	class_destroy(hello_class);
r_class:
	cdev_del(&hello_cdev);
r_cdev:
	unregister_chrdev_region(dev, 1);
r_region:
	gpio_free(LED_PIN);
	gpio_free(BTN_PIN);
	return -1;
}

static void __exit hello_exit(void)
{
	kthread_stop(led_thread);
	kthread_stop(morse_thread);
	device_destroy(hello_class, dev);
	class_destroy(hello_class);
	cdev_del(&hello_cdev);
	unregister_chrdev_region(dev, 1);
	gpio_free(LED_PIN);
	gpio_free(BTN_PIN);
	pr_info("Module unloaded succesfully!");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");

