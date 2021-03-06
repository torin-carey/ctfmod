#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <asm/bitops.h>
#include <linux/types.h>
#include <linux/sem.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/miscdevice.h>

#include <ctfmod.h>
#include <sha256.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Torin Carey <torin@tcarey.uk>");

static struct flag_key *secrets = NULL;

#define FLAG_FLAG2 1
#define FLAG_FLAG3 2

struct file_state {
	volatile unsigned long auth;
};

static const char mymessage[] = "Nothing to see here... (see ctfmod(4))\n";

static int mod_open(struct inode *inode, struct file *file)
{
	struct file_state *state;

	state = kmalloc(sizeof(struct file_state), GFP_KERNEL);
	if (!state) {
		printk(KERN_ERR "ctfmod: open: failed to allocate memory\n");
		return -ENOMEM;
	}
	state->auth = 0;
	file->private_data = (void *)state;
	return 0;
}

static int mod_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t mod_read(struct file *file, char __user *data, size_t size, loff_t *offset)
{
	unsigned int avail;
	unsigned long f;
	if (*offset >= sizeof(mymessage) || *offset < 0)
		return 0;
	avail = sizeof(mymessage) - *offset;
	avail = avail > size ? size : avail;
	f = copy_to_user(data, mymessage, avail);
	avail -= f;
	*offset += avail;
	return avail;
}

static ssize_t mod_write(struct file *file, const char __user *data, size_t size, loff_t *offset)
{
	return -EPERM;
}

static int authenticate(const struct auth_data *auth, const unsigned char key[16])
{
	struct sha256_state state;
	unsigned char digest[32];
	int ret;
	if (auth->message_len > sizeof(auth->message))
		return -EINVAL;
	sha256_init(&state);
	sha256_update(&state, key, 16);
	sha256_update(&state, auth->message, auth->message_len);
	sha256_final(&state, digest);
	ret = memcmp(digest, auth->digest, 32);
	return ret ? -EPERM : 0;
}

static long mod_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned long ret;
	struct flag_key sec, *global, *tmp;
	struct auth_data auth;
	struct file_state *state;
	void *sptr, *mptr, *eptr;
	kuid_t kuid;
	state = (struct file_state *)file->private_data;

	switch (cmd) {
	case CTFMOD_GET_FLAG1:
		rcu_read_lock();
		global = rcu_dereference(secrets);
		if (!global) {
			rcu_read_unlock();
			ret = -EBUSY;
			goto finish;
		}
		memcpy(sec.flag[0], global->flag[0], FLAG_LEN);
		rcu_read_unlock();
		if (copy_to_user((char *)arg, sec.flag[0], FLAG_LEN)) {
			ret = -EFAULT;
			goto finish;
		}
		ret = 0;
		goto finish;
	case CTFMOD_GET_FLAG2:
		if (!test_bit(FLAG_FLAG2, &state->auth)) {
			ret = -EPERM;
			goto finish;
		}
		rcu_read_lock();
		global = rcu_dereference(secrets);
		if (!global) {
			rcu_read_unlock();
			ret = -EBUSY;
			goto finish;
		}
		memcpy(sec.flag[1], global->flag[1], FLAG_LEN);
		rcu_read_unlock();
		if (copy_to_user((char *)arg, sec.flag[1], FLAG_LEN)) {
			ret = -EFAULT;
			goto finish;
		}
		ret = 0;
		goto finish;
	case CTFMOD_GET_FLAG3:
		if (!test_bit(FLAG_FLAG3, &state->auth)) {
			ret = -EPERM;
			goto finish;
		}
		rcu_read_lock();
		global = rcu_dereference(secrets);
		if (!global) {
			rcu_read_unlock();
			ret = -EBUSY;
			goto finish;
		}
		memcpy(sec.flag[2], global->flag[2], FLAG_LEN);
		rcu_read_unlock();
		if (copy_to_user((char *)arg, sec.flag[2], FLAG_LEN)) {
			ret = -EFAULT;
			goto finish;
		}
		ret = 0;
		goto finish;
	case CTFMOD_AUTHENTICATE:
		rcu_read_lock();
		global = rcu_dereference(secrets);
		if (!global) {
			rcu_read_unlock();
			ret = -EBUSY;
			goto finish;
		}
		memcpy(sec.key, global->key, 16);
		rcu_read_unlock();
		if (copy_from_user(&auth, (void *)arg, sizeof(struct auth_data))) {
			printk(KERN_INFO "ctfmod: failed to authenticate\n");
			ret = -EINVAL;
			goto finish;
		}
		ret = authenticate(&auth, sec.key);
		if (ret)
			goto finish;

		printk(KERN_DEBUG "ctfmod: data has correct mac\n");
		sptr = &auth.message;
		eptr = &auth.message[auth.message_len];
		while (sptr < eptr) {
			mptr = memchr(sptr, '\0', eptr - sptr);
			if (!mptr)
				mptr = eptr;

			if (mptr - sptr == 12) {
				if (!memcmp(sptr, "UNLOCK_FLAG2", 12)) {
					printk(KERN_INFO "ctfmod: flag2 unlocked\n");
					set_bit(FLAG_FLAG2, &state->auth);
				} else if (!memcmp(sptr, "UNLOCK_FLAG3", 12)) {
					printk(KERN_INFO "ctfmod: flag3 unlocked\n");
					set_bit(FLAG_FLAG3, &state->auth);
				}
			}
			sptr = mptr + 1;
		}

		ret = 0;
		goto finish;
	case CTFMOD_LOAD_SECRETS:
		printk(KERN_DEBUG "ctfmod: attempting LOAD_SECRETS\n");
		kuid = make_kuid(current_user_ns(), 0);
		if (!uid_eq(current_cred()->uid, kuid) && !capable(CAP_SYS_ADMIN)) {
			printk(KERN_DEBUG "ctfmod: LOAD_SECRETS failed: permission denied\n");
			ret = -EPERM;
			goto finish;
		}
		tmp = kmalloc(sizeof(struct flag_key), GFP_KERNEL);
		if (!tmp) {
			printk(KERN_ERR "ctfmod: LOAD_SECRETS failed: could not allocate memory\n");
			ret = -ENOMEM;
			goto finish;
		}
		if (copy_from_user(tmp, (void *)arg, sizeof(struct flag_key))) {
			printk(KERN_DEBUG "ctfmod: LOAD_SECRETS failed: could not obtain struct\n");
			ret = -EFAULT;
			goto finish;
		}
		printk(KERN_DEBUG "ctfmod: loaded flag1: %*pE\n", FLAG_LEN, tmp->flag[0]);
		printk(KERN_DEBUG "ctfmod: loaded flag2: %*pE\n", FLAG_LEN, tmp->flag[1]);
		printk(KERN_DEBUG "ctfmod: loaded flag3: %*pE\n", FLAG_LEN, tmp->flag[2]);
		printk(KERN_DEBUG "ctfmod: loaded key: %*ph\n", 16, tmp->key);
		global = rcu_dereference(secrets);
		rcu_assign_pointer(secrets, tmp);
		synchronize_rcu();
		kfree(global);
		ret = 0;
		printk(KERN_DEBUG "ctfmod: LOAD_SECRETS succeeded\n");
		goto finish;
	case CTFMOD_RETRIEVE_SECRETS:
		kuid = make_kuid(current_user_ns(), 0);
		if (!uid_eq(current_cred()->uid, kuid) && !capable(CAP_SYS_ADMIN)) {
			printk(KERN_DEBUG "ctfmod: RETRIEVE_SECRETS failed: permission denied\n");
			ret = -EPERM;
			goto finish;
		}
		rcu_read_lock();
		global = rcu_dereference(secrets);
		if (!global) {
			rcu_read_unlock();
			printk(KERN_DEBUG "ctfmod: RETRIEVE_SECRETS failed: not initialised\n");
			ret = -EBUSY;
			goto finish;
		}
		memcpy(&sec, global, sizeof(struct flag_key));
		rcu_read_unlock();
		if (copy_to_user((void *)arg, &sec, sizeof(struct flag_key))) {
			printk(KERN_DEBUG "ctfmod: RETRIEVE_SECRETS failed: could not write struct\n");
			ret = -EFAULT;
			goto finish;
		}

		ret = 0;
		printk(KERN_DEBUG "ctfmod: RETRIEVE_SECRETS succeeded\n");
		goto finish;
	case CTFMOD_CHECK_STATUS:
		rcu_read_lock();
		ret = rcu_dereference(secrets) ? 0 : -EBUSY;
		rcu_read_unlock();
		goto finish;
	default:
		ret = -ENOTTY;
		goto finish;
	}
finish:
	return ret;
}


static struct file_operations mod_fops = {
	.owner = THIS_MODULE,
	.open = mod_open,
	.release = mod_release,
	.read = mod_read,
	.write = mod_write,
	.unlocked_ioctl = mod_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice mod_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "flag",
	.fops = &mod_fops,
};

static int __init mod_init(void)
{
	int result;
	result = misc_register(&mod_miscdev);
	if (result < 0) {
		printk(KERN_ERR "ctfmod: initialisation failed: failed to register misc_device\n");
		return result;
	}
	printk(KERN_DEBUG "ctfmod: module loaded\n");
	return 0;
}

static void __exit mod_exit(void)
{
	if (secrets)
		kfree(secrets);
	misc_deregister(&mod_miscdev);
	printk(KERN_DEBUG "ctfmod: module unloaded\n");
}

module_init(mod_init);
module_exit(mod_exit);
