#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include "morse.h"

static struct proc_dir_entry *proc_file;

static ssize_t proc_read(struct file *file, char __user *ubuf, size_t count_req, loff_t *ppos) 
{
    char kbuf[128];
    int len;
    int current_count;

    if (*ppos > 0) return 0;

    mutex_lock(&buffer_mutex);
    // Logic: distance between head and tail
    current_count = (tail >= head) ? (tail - head) : (SIZE - head + tail);
    
    len = sprintf(kbuf, 
        "--- Morse Device Stats ---\n"
        "Head Position: %d\n"
        "Tail Position: %d\n"
        "Items in Buffer: %d\n", 
        head, tail, current_count);
    mutex_unlock(&buffer_mutex);

    if (copy_to_user(ubuf, kbuf, len))
        return -EFAULT;

    *ppos = len;
    return len;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops proc_fops = { .proc_read = proc_read };
#else
static const struct file_operations proc_fops = { .read = proc_read };
#endif

int morse_proc_init(void) {
    proc_file = proc_create("morsestats", 0444, NULL, &proc_fops);
    return proc_file ? 0 : -ENOMEM;
}

void morse_proc_cleanup(void) {
    proc_remove(proc_file);
}
