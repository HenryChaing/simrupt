/* simrupt: A device that simulates interrupts */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/spinlock.h> 

#include "game.h"
#include "negamax.h"
#include "mcts.h"
#include "util.h"
#include "zobrist.h"
#include "chardev.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("A device that simulates interrupts");

/* Macro DECLARE_TASKLET_OLD exists for compatibiity.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "simrupt3"

#define NR_SIMRUPT 1

static int delay = 100; /* time (in ms) to generate an event */

/* Data produced by the simulated device */
static int simrupt_data;

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;
static bool sel_work = true;
static int work_count = 0;

/* Character device stuff */
static int major;
static struct class *simrupt_class;
static struct cdev simrupt_cdev;
static bool pause = false;

/*ttt game static data*/
static int move_record[N_GRIDS];
static int move_count = 0;
static char table[N_GRIDS];
static char turn = 'X';
static char ai = 'O';


/*ttt game static method*/
static void record_move(int move)
{
    move_record[move_count++] = move;
}

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Generate new data from the simulated device */
static inline int update_simrupt_data(void)
{
    simrupt_data = max((simrupt_data + 1) % 0x7f, 0x20);
    return simrupt_data;
}

/* Insert a value into the kfifo buffer */
static void produce_data(unsigned char val)
{
    /* Implement a kind of circular FIFO here (skip oldest element if kfifo
     * buffer is full).
     */
    unsigned int len = kfifo_in(&rx_fifo, &val, sizeof(val));
    if (unlikely(len < sizeof(val)) && printk_ratelimit())
        pr_warn("%s: %zu bytes dropped\n", __func__, sizeof(val) - len);

    pr_debug("simrupt: %s: in %u/%u bytes\n", __func__, len,
             kfifo_len(&rx_fifo));
}

/* Mutex to serialize kfifo writers within the workqueue handler */
static DEFINE_MUTEX(producer_lock);
static DEFINE_MUTEX(work1_lock);
static DEFINE_MUTEX(work2_lock);
static DEFINE_MUTEX(process_lock);

/* Workqueue handler: executed by a kernel thread */
static void simrupt_work_func(struct work_struct *w)
{
    int cpu;

    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    
    /* Pretend to simulate access to per-CPU data, disabling preemption
     * during the pr_info().
     */
    cpu = get_cpu();
    pr_info("simrupt: [CPU#%d] %s\n", cpu, __func__);
    put_cpu();

    while (1) {
        mutex_lock(&work1_lock);
        /* Store data to the kfifo buffer */
        mutex_lock(&producer_lock);
        mutex_lock(&process_lock);
        char win = check_win(table);
        if (win == 'D') {
            pr_info("It is a draw!\n");
        } else if (win != ' ') {
            pr_info("%c won!\n", win);
        } else {

            int move = negamax_predict(table, ai).move;
            
            if (move != -1 ) {
                produce_data('0' + move);
                table[move] = ai;
                record_move(move);
            }
            pr_info("simrupt: [negamax] %d, %c\n",move,ai);
            
        }
        mutex_unlock(&process_lock);
        ssleep(2);
        
        mutex_unlock(&producer_lock);
        mutex_unlock(&work2_lock);
    }
    
    wake_up_interruptible(&rx_wait);
}

/* Workqueue handler: executed by a kernel thread */
static void simrupt_work_func2(struct work_struct *w)
{
    int cpu;

    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    
    /* Pretend to simulate access to per-CPU data, disabling preemption
     * during the pr_info().
     */
    cpu = get_cpu();
    pr_info("simrupt: [CPU#%d] %s\n", cpu, __func__);
    put_cpu();

    while (1) {
        mutex_lock(&work2_lock);
        /* Store data to the kfifo buffer */
        mutex_lock(&producer_lock);
        mutex_lock(&process_lock);
        char win = check_win(table);
        if (win == 'D') {
            pr_info("It is a draw!\n");
        } else if (win != ' ') {
            pr_info("%c won!\n", win);
        } else {
         
            int move = mcts(table, turn);
            if (move != -1) {
                produce_data('0' + move);
                table[move] = turn;
                record_move(move);
                
            }
            pr_info("simrupt: [negamax] %d, %c\n",move,turn);
            
        }
        mutex_unlock(&process_lock);
        ssleep(2);
        
        mutex_unlock(&producer_lock);
        mutex_unlock(&work1_lock);
    }
    
    wake_up_interruptible(&rx_wait);
}


/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *simrupt_workqueue;

/* Work item: holds a pointer to the function that is going to be executed
 * asynchronously.
 */

static DECLARE_WORK(work, simrupt_work_func);
static DECLARE_WORK(work2, simrupt_work_func2);

/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void simrupt_tasklet_func(unsigned long __data)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    WARN_ON_ONCE(!in_interrupt());
    WARN_ON_ONCE(!in_softirq());

    tv_start = ktime_get();
    if (sel_work && work_count < 20) {
        queue_work(simrupt_workqueue, &work);
        work_count ++;
        sel_work = false;
    } else if(work_count < 20){
        queue_work(simrupt_workqueue, &work2);
        work_count ++;
        sel_work = true;
    }
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("simrupt: [CPU#%d] %s in_softirq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
}

/* Tasklet for asynchronous bottom-half processing in softirq context */
static DECLARE_TASKLET_OLD(simrupt_tasklet, simrupt_tasklet_func);

static void process_data(void)
{
    WARN_ON_ONCE(!irqs_disabled());

    pr_info("simrupt: [CPU#%d] scheduling tasklet\n", smp_processor_id());
    tasklet_schedule(&simrupt_tasklet);
}

static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    pr_info("simrupt: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    /* We are using a kernel timer to simulate a hard-irq, so we must expect
     * to be in softirq context here.
     */
    WARN_ON_ONCE(!in_softirq());

    /* Disable interrupts for this CPU to simulate real interrupt context */
    local_irq_disable();

    tv_start = ktime_get();
    process_data();
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("simrupt: [CPU#%d] %s in_irq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
    mod_timer(&timer, jiffies + msecs_to_jiffies(delay));

    local_irq_enable();
}

static ssize_t simrupt_read(struct file *file,
                            char __user *buf,
                            size_t count,
                            loff_t *ppos)
{
    unsigned int read;
    int ret;

    pr_debug("simrupt: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock))
        return -ERESTARTSYS;

    do {
        char buffer[10];
        ret = kfifo_out(&rx_fifo, buffer, sizeof(buffer));
        buffer[ret] = '\0';
        if(!ret){
            break;
        }

        for (int i = 0; i <= ret; i++)
        {
            put_user(buffer[i], buf+i); 
        }
        break;
        /*
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        */
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
    } while (ret == 0);
    pr_debug("simrupt: %s: out %u/%u bytes\n", __func__, read,
             kfifo_len(&rx_fifo));

    mutex_unlock(&read_lock);

    return ret ? ret : read;
}

static ssize_t simrupt_write(struct file *file, const char __user *buffer, 
                            size_t length, loff_t *offset) 
{ 
    int i; 
 
    pr_info("device_write(%p,%p,%ld)", file, buffer, length); 
 
    for (i = 0; i < length && i < BUF_LEN; i++) 
        get_user(message[i], buffer + i); 
 
    if(message[0] == 'p' && !pause){
        mutex_lock(&process_lock);
        pause = true;
    } else if (message[0] == 'p' && pause){
        mutex_unlock(&process_lock);
        pause = false;
    }

    return i;
} 


/* This function is called whenever a process tries to do an ioctl on our 
 * device file. We get two extra parameters (additional to the inode and file 
 * structures, which all device functions get): the number of the ioctl called 
 * and the parameter given to the ioctl function. 
 * 
 * If the ioctl is write or read/write (meaning output is returned to the 
 * calling process), the ioctl call returns the output of this function. 
 */ 
static long 
device_ioctl(struct file *file, /* ditto */ 
             unsigned int ioctl_num, /* number and param for ioctl */ 
             unsigned long ioctl_param) 
{ 
    int i; 
    long ret = SUCCESS; 
 
    /* We don't want to talk to two processes at the same time. */ 
    if (atomic_cmpxchg(&already_open, CDEV_NOT_USED, CDEV_EXCLUSIVE_OPEN)) 
        return -EBUSY; 
 
    /* Switch according to the ioctl called */ 
    switch (ioctl_num) { 
    case IOCTL_SET_MSG: { 
        /* Receive a pointer to a message (in user space) and set that to 
         * be the device's message. Get the parameter given to ioctl by 
         * the process. 
         */ 
        char __user *tmp = (char __user *)ioctl_param; 
        char ch = 'A'; 
 
        /* Find the length of the message */  
        for (i = 0; ch && i < BUF_LEN; i++, tmp++) 
            get_user(ch, tmp); 
 
        simrupt_write(file, (char __user *)ioctl_param, i, NULL); 
        break; 
    } 
    case IOCTL_GET_MSG: { 
        loff_t offset = 0; 
 
        /* Give the current message to the calling process - the parameter 
         * we got is a pointer, fill it. 
         */ 
        i = simrupt_read(file, (char __user *)ioctl_param, 10, &offset); 
        
        /* Put a zero at the end of the buffer, so it will be properly 
         * terminated. 
         */ 
        put_user('\0', (char __user *)ioctl_param + i + 1); 
        break; 
    } 
    case IOCTL_GET_NTH_BYTE: 
        /* This ioctl is both input (ioctl_param) and output (the return 
         * value of this function). 
         */ 
        ret = (long)message[ioctl_param]; 
        break; 
    } 
 
    /* We're now ready for our next caller */ 
    atomic_set(&already_open, CDEV_NOT_USED); 
 
    return ret; 
} 

static atomic_t open_cnt;

static int simrupt_open(struct inode *inode, struct file *filp)
{
    pr_debug("simrupt: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    pr_info("openm current cnt: %d\n", atomic_read(&open_cnt));
    mutex_lock(&work2_lock);

    return 0;
}

static int simrupt_release(struct inode *inode, struct file *filp)
{
    pr_debug("simrupt: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt) == 0) {
        del_timer_sync(&timer);
        flush_workqueue(simrupt_workqueue);
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static const struct file_operations simrupt_fops = {
    .read = simrupt_read,
    .write = simrupt_write,
    .unlocked_ioctl = device_ioctl, 
    .llseek = no_llseek,
    .open = simrupt_open,
    .release = simrupt_release,
    .owner = THIS_MODULE,
};

static int __init simrupt_init(void)
{
    dev_t dev_id;
    int ret;

    if (kfifo_alloc(&rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, NR_SIMRUPT, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&simrupt_cdev, &simrupt_fops);
    ret = cdev_add(&simrupt_cdev, dev_id, NR_SIMRUPT);
    if (ret) {
        kobject_put(&simrupt_cdev.kobj);
        goto error_region;
    }

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    simrupt_class = class_create(THIS_MODULE, DEV_NAME);
#else
    simrupt_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(simrupt_class)) {
        printk(KERN_ERR "error creating simrupt class\n");
        ret = PTR_ERR(simrupt_class);
        goto error_cdev;
    }

    /* Register the device with sysfs */
    device_create(simrupt_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);

    /* Create the workqueue */
    simrupt_workqueue = alloc_workqueue("simruptd", WQ_UNBOUND, WQ_MAX_ACTIVE);
    // struct workqueue_attrs attrs;
	// BUG_ON(!(attrs = alloc_workqueue_attrs()));
	// attrs->nice = std_nice[i];

    // simrupt_workqueue2 = alloc_workqueue("simruptd", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!simrupt_workqueue) {
        device_destroy(simrupt_class, dev_id);
        class_destroy(simrupt_class);
        ret = -ENOMEM;
        goto error_cdev;
    }

    /* Setup the timer */
    timer_setup(&timer, timer_handler, 0);
    atomic_set(&open_cnt, 0);

    /* ttt game initial */
    memset(table, ' ', N_GRIDS);
    negamax_init();

    pr_info("simrupt: registered new simrupt device: %d,%d\n", major, 0);
out:
    return ret;
error_cdev:
    cdev_del(&simrupt_cdev);
error_region:
    unregister_chrdev_region(dev_id, NR_SIMRUPT);
error_alloc:
    kfifo_free(&rx_fifo);
    goto out;
}

static void __exit simrupt_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);

    del_timer_sync(&timer);
    tasklet_kill(&simrupt_tasklet);
    flush_workqueue(simrupt_workqueue);
    destroy_workqueue(simrupt_workqueue);
    device_destroy(simrupt_class, dev_id);
    class_destroy(simrupt_class);
    cdev_del(&simrupt_cdev);
    unregister_chrdev_region(dev_id, NR_SIMRUPT);

    kfifo_free(&rx_fifo);
    pr_info("simrupt: unloaded\n");
}

module_init(simrupt_init);
module_exit(simrupt_exit);
