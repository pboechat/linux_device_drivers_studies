#include <asm/uaccess.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>

MODULE_LICENSE("Dual BSD/GPL");

#define SCULL_NUM_DEVICES 4
#define SCULL_QUANTA_COUNT 1024
#define SCULL_QUANTUM_SIZE 1024
#define FMT_DEVICE_NUMBER "device %d %d"
#define DEVICE_NUMBER(dev) MAJOR(dev), MINOR(dev)

struct scull_qset
{
	void** quanta;
	struct scull_qset* next;
};

struct scull_device
{
	struct scull_qset* qsets;
	unsigned long size;
	struct cdev cdev;
};

// forward decls
static void scull_exit(void);
static int scull_init(void);
int scull_open(struct inode*, struct file*);
int scull_release(struct inode*, struct file*);
ssize_t scull_read(struct file*, char __user*, size_t, loff_t*);
ssize_t scull_write(struct file*, const char __user*, size_t, loff_t*);
void scull_find(loff_t*, int*, int*, int*);
void scull_trim(struct scull_device*);
struct scull_qset* scull_follow(struct scull_device*, int);

int scull_major;
int scull_minor = 0;
int scull_num_devices = SCULL_NUM_DEVICES;
int scull_quantum_size = SCULL_QUANTUM_SIZE;
int scull_quanta_count = SCULL_QUANTA_COUNT;
int scull_qset_size;
struct scull_device* scull_devices;
struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.open = scull_open,
	.release = scull_release,
	.read = scull_read,
	.write = scull_write
};

static int scull_init(void) 
{
	dev_t base_dev_num, dev_num;
	int result, i, cdev_err;

	scull_qset_size = scull_quanta_count * scull_quantum_size;

	result = alloc_chrdev_region(&base_dev_num, scull_minor, scull_num_devices, "scull");
	if (result < 0) {
		printk(KERN_WARNING "scull: can't allocate device regions\n");
		return result;
	}
	scull_major = MAJOR(base_dev_num);

	scull_devices = kmalloc(scull_num_devices * sizeof(struct scull_device), GFP_KERNEL);
	if (!scull_devices) {
		result = -ENOMEM;
		goto fail;
	}
	memset(scull_devices, 0, scull_num_devices * sizeof(struct scull_device));

	for (i = 0; i < scull_num_devices; ++i) {
		cdev_init(&scull_devices[i].cdev, &scull_fops);
		scull_devices[i].cdev.owner = THIS_MODULE;
		scull_devices[i].cdev.ops = &scull_fops;
		dev_num = MKDEV(scull_major, scull_minor + i);
		cdev_err = cdev_add(&scull_devices[i].cdev, dev_num, 1);
		if (cdev_err < 0) {
			printk(KERN_WARNING "scull: failed to add cdev\n");
		}
	}

	printk(KERN_INFO "scull: init\n");

	return 0;

fail:
	scull_exit();
	return result;
}

static void scull_exit() 
{
	int i;
	dev_t dev = MKDEV(scull_major, scull_minor);

	if (scull_devices) {
		for (i = 0; i < scull_num_devices; ++i) {
			cdev_del(&scull_devices[i].cdev);
		}
		kfree(scull_devices);
	}

	unregister_chrdev_region(dev, scull_num_devices);

	printk(KERN_INFO "scull: exit\n");
}

int scull_open(struct inode* inode, struct file* filp) 
{
	struct scull_device* dev;

	dev = container_of(inode->i_cdev, struct scull_device, cdev);
	filp->private_data = dev;

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		scull_trim(dev);
	}

	return 0;
}

int scull_release(struct inode* inode, struct file* filp) 
{
	return 0;
}

ssize_t scull_read(struct file* filp, char __user* buffer, size_t count, loff_t* f_pos) 
{
	struct scull_device* dev = filp->private_data;
	struct scull_qset* qset;
	int qset_idx, quantum_idx, quantum_pos;
	ssize_t retval = 0;

	if (*f_pos >= dev->size) 
		goto out;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	scull_find(f_pos, &qset_idx, &quantum_idx, &quantum_pos);

	qset = scull_follow(dev, qset_idx);

	if (qset == NULL || !qset->quanta || !qset->quanta[quantum_idx]) {
		printk(KERN_WARNING "scull: cannot read at position %lld (device " FMT_DEVICE_NUMBER ")\n", *f_pos, DEVICE_NUMBER(dev->cdev.dev));
		goto out;
	}

	// trim the read amount to the size of the quantum
	if (count > scull_quantum_size - quantum_pos)
		count = scull_quantum_size - quantum_pos;

	if (copy_to_user(buffer, qset->quanta[quantum_idx] + quantum_pos, count)) {
		printk(KERN_WARNING "scull: failed to copy into user-mode buffer (device " FMT_DEVICE_NUMBER ")\n", DEVICE_NUMBER(dev->cdev.dev));
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

	printk(KERN_DEBUG "scull: read %ld bytes (device " FMT_DEVICE_NUMBER ")\n", count, DEVICE_NUMBER(dev->cdev.dev));
out:
	return retval;
}

ssize_t scull_write(struct file* filp, const char __user* buffer, size_t count, loff_t* f_pos) 
{
	struct scull_device* dev = filp->private_data;
	struct scull_qset* qset;
	int qset_idx, quantum_idx, quantum_pos;
	ssize_t retval = -ENOMEM;

	scull_find(f_pos, &qset_idx, &quantum_idx, &quantum_pos);

	qset = scull_follow(dev, qset_idx);

	if (qset == NULL) {
		printk(KERN_WARNING "scull: cannot write at position %lld (device " FMT_DEVICE_NUMBER ")\n", *f_pos, DEVICE_NUMBER(dev->cdev.dev));
		goto out;
	}

	if (!qset->quanta) {
		qset->quanta = kmalloc(scull_quanta_count * sizeof(void*), GFP_KERNEL);
		if (!qset->quanta) {
			printk(KERN_WARNING "scull: cannot write at position %lld (device " FMT_DEVICE_NUMBER ")\n", *f_pos, DEVICE_NUMBER(dev->cdev.dev));
			goto out;
		}
		memset(qset->quanta, 0, scull_quanta_count * sizeof(void*));
	}

	if (!qset->quanta[quantum_idx]) {
		qset->quanta[quantum_idx] = kmalloc(scull_quantum_size, GFP_KERNEL);
		if (!qset->quanta[quantum_idx]) {
			goto out;
		}
		memset(qset->quanta[quantum_idx], 0, scull_quantum_size);
	}

	// trim the write amount to the size of the quantum
	if (count > scull_quantum_size - quantum_pos)
		count = scull_quantum_size - quantum_pos;

	if (copy_from_user(qset->quanta[quantum_idx] + quantum_pos, buffer, count)) {
		printk(KERN_WARNING "scull: failed to copy from user-mode buffer (device " FMT_DEVICE_NUMBER ")\n", DEVICE_NUMBER(dev->cdev.dev));
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

	printk(KERN_DEBUG "scull: wrote %ld bytes (device " FMT_DEVICE_NUMBER ")\n", count, DEVICE_NUMBER(dev->cdev.dev));

	if (dev->size < *f_pos)
		dev->size = *f_pos;
out:
	return retval;
}

void scull_find(loff_t* f_pos, int* qset_idx, int* quantum_idx, int* quantum_pos)
{
	int rest;
	*qset_idx = (long)*f_pos / scull_qset_size;
	rest = (long)*f_pos % scull_qset_size;
	*quantum_idx = rest / scull_quantum_size;
	*quantum_pos = rest % scull_quantum_size;
}

void scull_trim(struct scull_device* dev) 
{
	struct scull_qset* next_qset, *curr_qset;
	int i;
	for (curr_qset = dev->qsets; curr_qset; curr_qset = next_qset) {
		if (curr_qset->quanta) {
			for (i = 0; i < scull_quanta_count; ++i)
				kfree(curr_qset->quanta[i]);
			kfree(curr_qset->quanta);
			curr_qset->quanta = 0;
		}
		next_qset = curr_qset->next;
		kfree(curr_qset);
	}
	dev->size = 0;
	dev->qsets = 0;
}

struct scull_qset* scull_follow(struct scull_device* dev, int qset_idx)
{
	struct scull_qset* qset = dev->qsets;

	if (!qset) {
		qset = dev->qsets = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if (!qset)
			return NULL;
		memset(qset, 0, sizeof(struct scull_qset));
	}

	while (qset_idx--) {
		if (!qset->next) {
			qset = dev->qsets = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if (!qset)
				return NULL;
			memset(qset, 0, sizeof(struct scull_qset));
		}
		qset = qset-> next;
	}

	return qset;
}

module_init(scull_init);
module_exit(scull_exit);

