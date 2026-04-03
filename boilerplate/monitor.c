/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

struct monitor_entry {
    pid_t pid;
    char container_id[32];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_limit_triggered;
    struct list_head list;
};

static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * FIXED: Correct RSS Helper using get_mm_rss()
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;
    long rss_bytes = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        // CORRECT WAY: Use get_mm_rss() and convert to bytes
        rss_pages = get_mm_rss(mm);
        rss_bytes = rss_pages * PAGE_SIZE;
        mmput(mm);
    }
    put_task_struct(task);

    return rss_bytes;
}

static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld bytes limit=%lu bytes (rss=%lu MB limit=%lu MB)\n",
           container_id, pid, rss_bytes, limit_bytes, rss_bytes/(1024*1024), limit_bytes/(1024*1024));
}

static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        printk(KERN_WARNING
               "[container_monitor] HARD LIMIT - Sending SIGKILL to container=%s pid=%d rss=%ld bytes limit=%lu bytes (rss=%lu MB limit=%lu MB)\n",
               container_id, pid, rss_bytes, limit_bytes, rss_bytes/(1024*1024), limit_bytes/(1024*1024));
        send_sig(SIGKILL, task, 0);
    }
    rcu_read_unlock();
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *entry, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {

        long rss = get_rss_bytes(entry->pid);

        // Process exited
        if (rss < 0) {
            printk(KERN_INFO "[container_monitor] Process %d (container=%s) exited, removing from list\n",
                   entry->pid, entry->container_id);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        // Debug: Print current RSS (optional, can be removed)
        printk(KERN_DEBUG "[container_monitor] container=%s pid=%d rss=%lu bytes (%lu MB) soft_limit=%lu hard_limit=%lu\n",
               entry->container_id, entry->pid, rss, rss/(1024*1024),
               entry->soft_limit_bytes, entry->hard_limit_bytes);

        // Soft limit check
        if (rss > entry->soft_limit_bytes && !entry->soft_limit_triggered) {
            log_soft_limit_event(entry->container_id,
                                 entry->pid,
                                 entry->soft_limit_bytes,
                                 rss);
            entry->soft_limit_triggered = 1;
        }

        // Hard limit check
        if (rss > entry->hard_limit_bytes) {
            kill_process(entry->container_id,
                         entry->pid,
                         entry->hard_limit_bytes,
                         rss);
            list_del(&entry->list);
            kfree(entry);
        }
    }

    mutex_unlock(&monitor_lock);

    // CRITICAL: Reschedule timer to run again
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitor_entry *entry, *tmp;
    int found = 0;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu bytes (%lu MB) hard=%lu bytes (%lu MB)\n",
               req.container_id, req.pid, 
               req.soft_limit_bytes, req.soft_limit_bytes/(1024*1024),
               req.hard_limit_bytes, req.hard_limit_bytes/(1024*1024));

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        strncpy(entry->container_id, req.container_id, sizeof(entry->container_id) - 1);
        entry->container_id[sizeof(entry->container_id) - 1] = '\0';
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_limit_triggered = 0;

        mutex_lock(&monitor_lock);
        list_add(&entry->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        printk(KERN_INFO "[container_monitor] Successfully registered %s (pid=%d)\n", 
               req.container_id, req.pid);
        return 0;
    }

    if (cmd == MONITOR_UNREGISTER) {
        printk(KERN_INFO
               "[container_monitor] Unregister request container=%s pid=%d\n",
               req.container_id, req.pid);

        mutex_lock(&monitor_lock);
        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                printk(KERN_INFO
                       "[container_monitor] Removed container=%s pid=%d\n",
                       req.container_id, req.pid);
                break;
            }
        }
        mutex_unlock(&monitor_lock);
        
        return found ? 0 : -ENOENT;
    }

    return -EINVAL;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void)
{

    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    // Initialize timer
    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    printk(KERN_INFO "[container_monitor] Timer started, checking every %d second(s)\n", CHECK_INTERVAL_SEC);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct monitor_entry *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");