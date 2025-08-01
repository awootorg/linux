// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/sunrpc/rpc_pipe.c
 *
 * Userland/kernel interface for rpcauth_gss.
 * Code shamelessly plagiarized from fs/nfsd/nfsctl.c
 * and fs/sysfs/inode.c
 *
 * Copyright (c) 2002, Trond Myklebust <trond.myklebust@fys.uio.no>
 *
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/pagemap.h>
#include <linux/mount.h>
#include <linux/fs_context.h>
#include <linux/namei.h>
#include <linux/fsnotify.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/utsname.h>

#include <asm/ioctls.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/seq_file.h>

#include <linux/sunrpc/clnt.h>
#include <linux/workqueue.h>
#include <linux/sunrpc/rpc_pipe_fs.h>
#include <linux/sunrpc/cache.h>
#include <linux/nsproxy.h>
#include <linux/notifier.h>

#include "netns.h"
#include "sunrpc.h"

#define RPCDBG_FACILITY RPCDBG_DEBUG

#define NET_NAME(net)	((net == &init_net) ? " (init_net)" : "")

static struct file_system_type rpc_pipe_fs_type;
static const struct rpc_pipe_ops gssd_dummy_pipe_ops;

static struct kmem_cache *rpc_inode_cachep __read_mostly;

#define RPC_UPCALL_TIMEOUT (30*HZ)

static BLOCKING_NOTIFIER_HEAD(rpc_pipefs_notifier_list);

int rpc_pipefs_notifier_register(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&rpc_pipefs_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(rpc_pipefs_notifier_register);

void rpc_pipefs_notifier_unregister(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&rpc_pipefs_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(rpc_pipefs_notifier_unregister);

static void rpc_purge_list(wait_queue_head_t *waitq, struct list_head *head,
		void (*destroy_msg)(struct rpc_pipe_msg *), int err)
{
	struct rpc_pipe_msg *msg;

	if (list_empty(head))
		return;
	do {
		msg = list_entry(head->next, struct rpc_pipe_msg, list);
		list_del_init(&msg->list);
		msg->errno = err;
		destroy_msg(msg);
	} while (!list_empty(head));

	if (waitq)
		wake_up(waitq);
}

static void
rpc_timeout_upcall_queue(struct work_struct *work)
{
	LIST_HEAD(free_list);
	struct rpc_pipe *pipe =
		container_of(work, struct rpc_pipe, queue_timeout.work);
	void (*destroy_msg)(struct rpc_pipe_msg *);
	struct dentry *dentry;

	spin_lock(&pipe->lock);
	destroy_msg = pipe->ops->destroy_msg;
	if (pipe->nreaders == 0) {
		list_splice_init(&pipe->pipe, &free_list);
		pipe->pipelen = 0;
	}
	dentry = dget(pipe->dentry);
	spin_unlock(&pipe->lock);
	rpc_purge_list(dentry ? &RPC_I(d_inode(dentry))->waitq : NULL,
			&free_list, destroy_msg, -ETIMEDOUT);
	dput(dentry);
}

ssize_t rpc_pipe_generic_upcall(struct file *filp, struct rpc_pipe_msg *msg,
				char __user *dst, size_t buflen)
{
	char *data = (char *)msg->data + msg->copied;
	size_t mlen = min(msg->len - msg->copied, buflen);
	unsigned long left;

	left = copy_to_user(dst, data, mlen);
	if (left == mlen) {
		msg->errno = -EFAULT;
		return -EFAULT;
	}

	mlen -= left;
	msg->copied += mlen;
	msg->errno = 0;
	return mlen;
}
EXPORT_SYMBOL_GPL(rpc_pipe_generic_upcall);

/**
 * rpc_queue_upcall - queue an upcall message to userspace
 * @pipe: upcall pipe on which to queue given message
 * @msg: message to queue
 *
 * Call with an @inode created by rpc_mkpipe() to queue an upcall.
 * A userspace process may then later read the upcall by performing a
 * read on an open file for this inode.  It is up to the caller to
 * initialize the fields of @msg (other than @msg->list) appropriately.
 */
int
rpc_queue_upcall(struct rpc_pipe *pipe, struct rpc_pipe_msg *msg)
{
	int res = -EPIPE;
	struct dentry *dentry;

	spin_lock(&pipe->lock);
	if (pipe->nreaders) {
		list_add_tail(&msg->list, &pipe->pipe);
		pipe->pipelen += msg->len;
		res = 0;
	} else if (pipe->flags & RPC_PIPE_WAIT_FOR_OPEN) {
		if (list_empty(&pipe->pipe))
			queue_delayed_work(rpciod_workqueue,
					&pipe->queue_timeout,
					RPC_UPCALL_TIMEOUT);
		list_add_tail(&msg->list, &pipe->pipe);
		pipe->pipelen += msg->len;
		res = 0;
	}
	dentry = dget(pipe->dentry);
	spin_unlock(&pipe->lock);
	if (dentry) {
		wake_up(&RPC_I(d_inode(dentry))->waitq);
		dput(dentry);
	}
	return res;
}
EXPORT_SYMBOL_GPL(rpc_queue_upcall);

static inline void
rpc_inode_setowner(struct inode *inode, void *private)
{
	RPC_I(inode)->private = private;
}

static void
rpc_close_pipes(struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct rpc_pipe *pipe = RPC_I(inode)->pipe;
	int need_release;
	LIST_HEAD(free_list);

	inode_lock(inode);
	spin_lock(&pipe->lock);
	need_release = pipe->nreaders != 0 || pipe->nwriters != 0;
	pipe->nreaders = 0;
	list_splice_init(&pipe->in_upcall, &free_list);
	list_splice_init(&pipe->pipe, &free_list);
	pipe->pipelen = 0;
	pipe->dentry = NULL;
	spin_unlock(&pipe->lock);
	rpc_purge_list(&RPC_I(inode)->waitq, &free_list, pipe->ops->destroy_msg, -EPIPE);
	pipe->nwriters = 0;
	if (need_release && pipe->ops->release_pipe)
		pipe->ops->release_pipe(inode);
	cancel_delayed_work_sync(&pipe->queue_timeout);
	rpc_inode_setowner(inode, NULL);
	RPC_I(inode)->pipe = NULL;
	inode_unlock(inode);
}

static struct inode *
rpc_alloc_inode(struct super_block *sb)
{
	struct rpc_inode *rpci;
	rpci = alloc_inode_sb(sb, rpc_inode_cachep, GFP_KERNEL);
	if (!rpci)
		return NULL;
	return &rpci->vfs_inode;
}

static void
rpc_free_inode(struct inode *inode)
{
	kmem_cache_free(rpc_inode_cachep, RPC_I(inode));
}

static int
rpc_pipe_open(struct inode *inode, struct file *filp)
{
	struct rpc_pipe *pipe;
	int first_open;
	int res = -ENXIO;

	inode_lock(inode);
	pipe = RPC_I(inode)->pipe;
	if (pipe == NULL)
		goto out;
	first_open = pipe->nreaders == 0 && pipe->nwriters == 0;
	if (first_open && pipe->ops->open_pipe) {
		res = pipe->ops->open_pipe(inode);
		if (res)
			goto out;
	}
	if (filp->f_mode & FMODE_READ)
		pipe->nreaders++;
	if (filp->f_mode & FMODE_WRITE)
		pipe->nwriters++;
	res = 0;
out:
	inode_unlock(inode);
	return res;
}

static int
rpc_pipe_release(struct inode *inode, struct file *filp)
{
	struct rpc_pipe *pipe;
	struct rpc_pipe_msg *msg;
	int last_close;

	inode_lock(inode);
	pipe = RPC_I(inode)->pipe;
	if (pipe == NULL)
		goto out;
	msg = filp->private_data;
	if (msg != NULL) {
		spin_lock(&pipe->lock);
		msg->errno = -EAGAIN;
		list_del_init(&msg->list);
		spin_unlock(&pipe->lock);
		pipe->ops->destroy_msg(msg);
	}
	if (filp->f_mode & FMODE_WRITE)
		pipe->nwriters --;
	if (filp->f_mode & FMODE_READ) {
		pipe->nreaders --;
		if (pipe->nreaders == 0) {
			LIST_HEAD(free_list);
			spin_lock(&pipe->lock);
			list_splice_init(&pipe->pipe, &free_list);
			pipe->pipelen = 0;
			spin_unlock(&pipe->lock);
			rpc_purge_list(&RPC_I(inode)->waitq, &free_list,
					pipe->ops->destroy_msg, -EAGAIN);
		}
	}
	last_close = pipe->nwriters == 0 && pipe->nreaders == 0;
	if (last_close && pipe->ops->release_pipe)
		pipe->ops->release_pipe(inode);
out:
	inode_unlock(inode);
	return 0;
}

static ssize_t
rpc_pipe_read(struct file *filp, char __user *buf, size_t len, loff_t *offset)
{
	struct inode *inode = file_inode(filp);
	struct rpc_pipe *pipe;
	struct rpc_pipe_msg *msg;
	int res = 0;

	inode_lock(inode);
	pipe = RPC_I(inode)->pipe;
	if (pipe == NULL) {
		res = -EPIPE;
		goto out_unlock;
	}
	msg = filp->private_data;
	if (msg == NULL) {
		spin_lock(&pipe->lock);
		if (!list_empty(&pipe->pipe)) {
			msg = list_entry(pipe->pipe.next,
					struct rpc_pipe_msg,
					list);
			list_move(&msg->list, &pipe->in_upcall);
			pipe->pipelen -= msg->len;
			filp->private_data = msg;
			msg->copied = 0;
		}
		spin_unlock(&pipe->lock);
		if (msg == NULL)
			goto out_unlock;
	}
	/* NOTE: it is up to the callback to update msg->copied */
	res = pipe->ops->upcall(filp, msg, buf, len);
	if (res < 0 || msg->len == msg->copied) {
		filp->private_data = NULL;
		spin_lock(&pipe->lock);
		list_del_init(&msg->list);
		spin_unlock(&pipe->lock);
		pipe->ops->destroy_msg(msg);
	}
out_unlock:
	inode_unlock(inode);
	return res;
}

static ssize_t
rpc_pipe_write(struct file *filp, const char __user *buf, size_t len, loff_t *offset)
{
	struct inode *inode = file_inode(filp);
	int res;

	inode_lock(inode);
	res = -EPIPE;
	if (RPC_I(inode)->pipe != NULL)
		res = RPC_I(inode)->pipe->ops->downcall(filp, buf, len);
	inode_unlock(inode);
	return res;
}

static __poll_t
rpc_pipe_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct inode *inode = file_inode(filp);
	struct rpc_inode *rpci = RPC_I(inode);
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(filp, &rpci->waitq, wait);

	inode_lock(inode);
	if (rpci->pipe == NULL)
		mask |= EPOLLERR | EPOLLHUP;
	else if (filp->private_data || !list_empty(&rpci->pipe->pipe))
		mask |= EPOLLIN | EPOLLRDNORM;
	inode_unlock(inode);
	return mask;
}

static long
rpc_pipe_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct rpc_pipe *pipe;
	int len;

	switch (cmd) {
	case FIONREAD:
		inode_lock(inode);
		pipe = RPC_I(inode)->pipe;
		if (pipe == NULL) {
			inode_unlock(inode);
			return -EPIPE;
		}
		spin_lock(&pipe->lock);
		len = pipe->pipelen;
		if (filp->private_data) {
			struct rpc_pipe_msg *msg;
			msg = filp->private_data;
			len += msg->len - msg->copied;
		}
		spin_unlock(&pipe->lock);
		inode_unlock(inode);
		return put_user(len, (int __user *)arg);
	default:
		return -EINVAL;
	}
}

static const struct file_operations rpc_pipe_fops = {
	.owner		= THIS_MODULE,
	.read		= rpc_pipe_read,
	.write		= rpc_pipe_write,
	.poll		= rpc_pipe_poll,
	.unlocked_ioctl	= rpc_pipe_ioctl,
	.open		= rpc_pipe_open,
	.release	= rpc_pipe_release,
};

static int
rpc_show_info(struct seq_file *m, void *v)
{
	struct rpc_clnt *clnt = m->private;

	rcu_read_lock();
	seq_printf(m, "RPC server: %s\n",
			rcu_dereference(clnt->cl_xprt)->servername);
	seq_printf(m, "service: %s (%d) version %d\n", clnt->cl_program->name,
			clnt->cl_prog, clnt->cl_vers);
	seq_printf(m, "address: %s\n", rpc_peeraddr2str(clnt, RPC_DISPLAY_ADDR));
	seq_printf(m, "protocol: %s\n", rpc_peeraddr2str(clnt, RPC_DISPLAY_PROTO));
	seq_printf(m, "port: %s\n", rpc_peeraddr2str(clnt, RPC_DISPLAY_PORT));
	rcu_read_unlock();
	return 0;
}

static int
rpc_info_open(struct inode *inode, struct file *file)
{
	struct rpc_clnt *clnt = NULL;
	int ret = single_open(file, rpc_show_info, NULL);

	if (!ret) {
		struct seq_file *m = file->private_data;

		spin_lock(&file->f_path.dentry->d_lock);
		if (!d_unhashed(file->f_path.dentry))
			clnt = RPC_I(inode)->private;
		if (clnt != NULL && refcount_inc_not_zero(&clnt->cl_count)) {
			spin_unlock(&file->f_path.dentry->d_lock);
			m->private = clnt;
		} else {
			spin_unlock(&file->f_path.dentry->d_lock);
			single_release(inode, file);
			ret = -EINVAL;
		}
	}
	return ret;
}

static int
rpc_info_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct rpc_clnt *clnt = (struct rpc_clnt *)m->private;

	if (clnt)
		rpc_release_client(clnt);
	return single_release(inode, file);
}

static const struct file_operations rpc_info_operations = {
	.owner		= THIS_MODULE,
	.open		= rpc_info_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= rpc_info_release,
};


/*
 * Description of fs contents.
 */
struct rpc_filelist {
	const char *name;
	const struct file_operations *i_fop;
	umode_t mode;
};

static struct inode *
rpc_get_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode = new_inode(sb);
	if (!inode)
		return NULL;
	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	simple_inode_init_ts(inode);
	switch (mode & S_IFMT) {
	case S_IFDIR:
		inode->i_fop = &simple_dir_operations;
		inode->i_op = &simple_dir_inode_operations;
		inc_nlink(inode);
		break;
	default:
		break;
	}
	return inode;
}

static void
init_pipe(struct rpc_pipe *pipe)
{
	pipe->nreaders = 0;
	pipe->nwriters = 0;
	INIT_LIST_HEAD(&pipe->in_upcall);
	INIT_LIST_HEAD(&pipe->in_downcall);
	INIT_LIST_HEAD(&pipe->pipe);
	pipe->pipelen = 0;
	INIT_DELAYED_WORK(&pipe->queue_timeout,
			    rpc_timeout_upcall_queue);
	pipe->ops = NULL;
	spin_lock_init(&pipe->lock);
	pipe->dentry = NULL;
}

void rpc_destroy_pipe_data(struct rpc_pipe *pipe)
{
	kfree(pipe);
}
EXPORT_SYMBOL_GPL(rpc_destroy_pipe_data);

struct rpc_pipe *rpc_mkpipe_data(const struct rpc_pipe_ops *ops, int flags)
{
	struct rpc_pipe *pipe;

	pipe = kzalloc(sizeof(struct rpc_pipe), GFP_KERNEL);
	if (!pipe)
		return ERR_PTR(-ENOMEM);
	init_pipe(pipe);
	pipe->ops = ops;
	pipe->flags = flags;
	return pipe;
}
EXPORT_SYMBOL_GPL(rpc_mkpipe_data);

static int rpc_new_file(struct dentry *parent,
			   const char *name,
			   umode_t mode,
			   const struct file_operations *i_fop,
			   void *private)
{
	struct dentry *dentry = simple_start_creating(parent, name);
	struct inode *dir = parent->d_inode;
	struct inode *inode;

	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	inode = rpc_get_inode(dir->i_sb, S_IFREG | mode);
	if (unlikely(!inode)) {
		dput(dentry);
		inode_unlock(dir);
		return -ENOMEM;
	}
	inode->i_ino = iunique(dir->i_sb, 100);
	if (i_fop)
		inode->i_fop = i_fop;
	rpc_inode_setowner(inode, private);
	d_instantiate(dentry, inode);
	fsnotify_create(dir, dentry);
	inode_unlock(dir);
	return 0;
}

static struct dentry *rpc_new_dir(struct dentry *parent,
				  const char *name,
				  umode_t mode)
{
	struct dentry *dentry = simple_start_creating(parent, name);
	struct inode *dir = parent->d_inode;
	struct inode *inode;

	if (IS_ERR(dentry))
		return dentry;

	inode = rpc_get_inode(dir->i_sb, S_IFDIR | mode);
	if (unlikely(!inode)) {
		dput(dentry);
		inode_unlock(dir);
		return ERR_PTR(-ENOMEM);
	}

	inode->i_ino = iunique(dir->i_sb, 100);
	inc_nlink(dir);
	d_instantiate(dentry, inode);
	fsnotify_mkdir(dir, dentry);
	inode_unlock(dir);

	return dentry;
}

static int rpc_populate(struct dentry *parent,
			const struct rpc_filelist *files,
			int start, int eof,
			void *private)
{
	struct dentry *dentry;
	int i, err;

	for (i = start; i < eof; i++) {
		switch (files[i].mode & S_IFMT) {
			default:
				BUG();
			case S_IFREG:
				err = rpc_new_file(parent,
						files[i].name,
						files[i].mode,
						files[i].i_fop,
						private);
				if (err)
					goto out_bad;
				break;
			case S_IFDIR:
				dentry = rpc_new_dir(parent,
						files[i].name,
						files[i].mode);
				if (IS_ERR(dentry)) {
					err = PTR_ERR(dentry);
					goto out_bad;
				}
		}
	}
	return 0;
out_bad:
	printk(KERN_WARNING "%s: %s failed to populate directory %pd\n",
			__FILE__, __func__, parent);
	return err;
}

/**
 * rpc_mkpipe_dentry - make an rpc_pipefs file for kernel<->userspace
 *		       communication
 * @parent: dentry of directory to create new "pipe" in
 * @name: name of pipe
 * @private: private data to associate with the pipe, for the caller's use
 * @pipe: &rpc_pipe containing input parameters
 *
 * Data is made available for userspace to read by calls to
 * rpc_queue_upcall().  The actual reads will result in calls to
 * @ops->upcall, which will be called with the file pointer,
 * message, and userspace buffer to copy to.
 *
 * Writes can come at any time, and do not necessarily have to be
 * responses to upcalls.  They will result in calls to @msg->downcall.
 *
 * The @private argument passed here will be available to all these methods
 * from the file pointer, via RPC_I(file_inode(file))->private.
 */
int rpc_mkpipe_dentry(struct dentry *parent, const char *name,
				 void *private, struct rpc_pipe *pipe)
{
	struct inode *dir = d_inode(parent);
	struct dentry *dentry;
	struct inode *inode;
	struct rpc_inode *rpci;
	umode_t umode = S_IFIFO | 0600;
	int err;

	if (pipe->ops->upcall == NULL)
		umode &= ~0444;
	if (pipe->ops->downcall == NULL)
		umode &= ~0222;

	dentry = simple_start_creating(parent, name);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		goto failed;
	}

	inode = rpc_get_inode(dir->i_sb, umode);
	if (unlikely(!inode)) {
		dput(dentry);
		inode_unlock(dir);
		err = -ENOMEM;
		goto failed;
	}
	inode->i_ino = iunique(dir->i_sb, 100);
	inode->i_fop = &rpc_pipe_fops;
	rpci = RPC_I(inode);
	rpci->private = private;
	rpci->pipe = pipe;
	rpc_inode_setowner(inode, private);
	d_instantiate(dentry, inode);
	pipe->dentry = dentry;
	fsnotify_create(dir, dentry);
	inode_unlock(dir);
	return 0;

failed:
	pr_warn("%s() failed to create pipe %pd/%s (errno = %d)\n",
			__func__, parent, name, err);
	return err;
}
EXPORT_SYMBOL_GPL(rpc_mkpipe_dentry);

/**
 * rpc_unlink - remove a pipe
 * @pipe: the pipe to be removed
 *
 * After this call, lookups will no longer find the pipe, and any
 * attempts to read or write using preexisting opens of the pipe will
 * return -EPIPE.
 */
void
rpc_unlink(struct rpc_pipe *pipe)
{
	if (pipe->dentry) {
		simple_recursive_removal(pipe->dentry, rpc_close_pipes);
		pipe->dentry = NULL;
	}
}
EXPORT_SYMBOL_GPL(rpc_unlink);

/**
 * rpc_init_pipe_dir_head - initialise a struct rpc_pipe_dir_head
 * @pdh: pointer to struct rpc_pipe_dir_head
 */
void rpc_init_pipe_dir_head(struct rpc_pipe_dir_head *pdh)
{
	INIT_LIST_HEAD(&pdh->pdh_entries);
	pdh->pdh_dentry = NULL;
}
EXPORT_SYMBOL_GPL(rpc_init_pipe_dir_head);

/**
 * rpc_init_pipe_dir_object - initialise a struct rpc_pipe_dir_object
 * @pdo: pointer to struct rpc_pipe_dir_object
 * @pdo_ops: pointer to const struct rpc_pipe_dir_object_ops
 * @pdo_data: pointer to caller-defined data
 */
void rpc_init_pipe_dir_object(struct rpc_pipe_dir_object *pdo,
		const struct rpc_pipe_dir_object_ops *pdo_ops,
		void *pdo_data)
{
	INIT_LIST_HEAD(&pdo->pdo_head);
	pdo->pdo_ops = pdo_ops;
	pdo->pdo_data = pdo_data;
}
EXPORT_SYMBOL_GPL(rpc_init_pipe_dir_object);

static int
rpc_add_pipe_dir_object_locked(struct net *net,
		struct rpc_pipe_dir_head *pdh,
		struct rpc_pipe_dir_object *pdo)
{
	int ret = 0;

	if (pdh->pdh_dentry)
		ret = pdo->pdo_ops->create(pdh->pdh_dentry, pdo);
	if (ret == 0)
		list_add_tail(&pdo->pdo_head, &pdh->pdh_entries);
	return ret;
}

static void
rpc_remove_pipe_dir_object_locked(struct net *net,
		struct rpc_pipe_dir_head *pdh,
		struct rpc_pipe_dir_object *pdo)
{
	if (pdh->pdh_dentry)
		pdo->pdo_ops->destroy(pdh->pdh_dentry, pdo);
	list_del_init(&pdo->pdo_head);
}

/**
 * rpc_add_pipe_dir_object - associate a rpc_pipe_dir_object to a directory
 * @net: pointer to struct net
 * @pdh: pointer to struct rpc_pipe_dir_head
 * @pdo: pointer to struct rpc_pipe_dir_object
 *
 */
int
rpc_add_pipe_dir_object(struct net *net,
		struct rpc_pipe_dir_head *pdh,
		struct rpc_pipe_dir_object *pdo)
{
	int ret = 0;

	if (list_empty(&pdo->pdo_head)) {
		struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

		mutex_lock(&sn->pipefs_sb_lock);
		ret = rpc_add_pipe_dir_object_locked(net, pdh, pdo);
		mutex_unlock(&sn->pipefs_sb_lock);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(rpc_add_pipe_dir_object);

/**
 * rpc_remove_pipe_dir_object - remove a rpc_pipe_dir_object from a directory
 * @net: pointer to struct net
 * @pdh: pointer to struct rpc_pipe_dir_head
 * @pdo: pointer to struct rpc_pipe_dir_object
 *
 */
void
rpc_remove_pipe_dir_object(struct net *net,
		struct rpc_pipe_dir_head *pdh,
		struct rpc_pipe_dir_object *pdo)
{
	if (!list_empty(&pdo->pdo_head)) {
		struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

		mutex_lock(&sn->pipefs_sb_lock);
		rpc_remove_pipe_dir_object_locked(net, pdh, pdo);
		mutex_unlock(&sn->pipefs_sb_lock);
	}
}
EXPORT_SYMBOL_GPL(rpc_remove_pipe_dir_object);

/**
 * rpc_find_or_alloc_pipe_dir_object
 * @net: pointer to struct net
 * @pdh: pointer to struct rpc_pipe_dir_head
 * @match: match struct rpc_pipe_dir_object to data
 * @alloc: allocate a new struct rpc_pipe_dir_object
 * @data: user defined data for match() and alloc()
 *
 */
struct rpc_pipe_dir_object *
rpc_find_or_alloc_pipe_dir_object(struct net *net,
		struct rpc_pipe_dir_head *pdh,
		int (*match)(struct rpc_pipe_dir_object *, void *),
		struct rpc_pipe_dir_object *(*alloc)(void *),
		void *data)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct rpc_pipe_dir_object *pdo;

	mutex_lock(&sn->pipefs_sb_lock);
	list_for_each_entry(pdo, &pdh->pdh_entries, pdo_head) {
		if (!match(pdo, data))
			continue;
		goto out;
	}
	pdo = alloc(data);
	if (!pdo)
		goto out;
	rpc_add_pipe_dir_object_locked(net, pdh, pdo);
out:
	mutex_unlock(&sn->pipefs_sb_lock);
	return pdo;
}
EXPORT_SYMBOL_GPL(rpc_find_or_alloc_pipe_dir_object);

static void
rpc_create_pipe_dir_objects(struct rpc_pipe_dir_head *pdh)
{
	struct rpc_pipe_dir_object *pdo;
	struct dentry *dir = pdh->pdh_dentry;

	list_for_each_entry(pdo, &pdh->pdh_entries, pdo_head)
		pdo->pdo_ops->create(dir, pdo);
}

static void
rpc_destroy_pipe_dir_objects(struct rpc_pipe_dir_head *pdh)
{
	struct rpc_pipe_dir_object *pdo;
	struct dentry *dir = pdh->pdh_dentry;

	list_for_each_entry(pdo, &pdh->pdh_entries, pdo_head)
		pdo->pdo_ops->destroy(dir, pdo);
}

/**
 * rpc_create_client_dir - Create a new rpc_client directory in rpc_pipefs
 * @dentry: the parent of new directory
 * @name: the name of new directory
 * @rpc_client: rpc client to associate with this directory
 *
 * This creates a directory at the given @path associated with
 * @rpc_clnt, which will contain a file named "info" with some basic
 * information about the client, together with any "pipes" that may
 * later be created using rpc_mkpipe().
 */
int rpc_create_client_dir(struct dentry *dentry,
			   const char *name,
			   struct rpc_clnt *rpc_client)
{
	struct dentry *ret;
	int err;

	ret = rpc_new_dir(dentry, name, 0555);
	if (IS_ERR(ret))
		return PTR_ERR(ret);
	err = rpc_new_file(ret, "info", S_IFREG | 0400,
			      &rpc_info_operations, rpc_client);
	if (err) {
		pr_warn("%s failed to populate directory %pd\n",
				__func__, ret);
		simple_recursive_removal(ret, NULL);
		return err;
	}
	rpc_client->cl_pipedir_objects.pdh_dentry = ret;
	rpc_create_pipe_dir_objects(&rpc_client->cl_pipedir_objects);
	return 0;
}

/**
 * rpc_remove_client_dir - Remove a directory created with rpc_create_client_dir()
 * @rpc_client: rpc_client for the pipe
 */
int rpc_remove_client_dir(struct rpc_clnt *rpc_client)
{
	struct dentry *dentry = rpc_client->cl_pipedir_objects.pdh_dentry;

	if (dentry == NULL)
		return 0;
	rpc_destroy_pipe_dir_objects(&rpc_client->cl_pipedir_objects);
	rpc_client->cl_pipedir_objects.pdh_dentry = NULL;
	simple_recursive_removal(dentry, NULL);
	return 0;
}

static const struct rpc_filelist cache_pipefs_files[3] = {
	[0] = {
		.name = "channel",
		.i_fop = &cache_file_operations_pipefs,
		.mode = S_IFREG | 0600,
	},
	[1] = {
		.name = "content",
		.i_fop = &content_file_operations_pipefs,
		.mode = S_IFREG | 0400,
	},
	[2] = {
		.name = "flush",
		.i_fop = &cache_flush_operations_pipefs,
		.mode = S_IFREG | 0600,
	},
};

struct dentry *rpc_create_cache_dir(struct dentry *parent, const char *name,
				    umode_t umode, struct cache_detail *cd)
{
	struct dentry *dentry;

	dentry = rpc_new_dir(parent, name, umode);
	if (!IS_ERR(dentry)) {
		int error = rpc_populate(dentry, cache_pipefs_files, 0, 3, cd);
		if (error) {
			simple_recursive_removal(dentry, NULL);
			return ERR_PTR(error);
		}
	}
	return dentry;
}

void rpc_remove_cache_dir(struct dentry *dentry)
{
	simple_recursive_removal(dentry, NULL);
}

/*
 * populate the filesystem
 */
static const struct super_operations s_ops = {
	.alloc_inode	= rpc_alloc_inode,
	.free_inode	= rpc_free_inode,
	.statfs		= simple_statfs,
};

#define RPCAUTH_GSSMAGIC 0x67596969

/*
 * We have a single directory with 1 node in it.
 */
enum {
	RPCAUTH_lockd,
	RPCAUTH_mount,
	RPCAUTH_nfs,
	RPCAUTH_portmap,
	RPCAUTH_statd,
	RPCAUTH_nfsd4_cb,
	RPCAUTH_cache,
	RPCAUTH_nfsd,
	RPCAUTH_RootEOF
};

static const struct rpc_filelist files[] = {
	[RPCAUTH_lockd] = {
		.name = "lockd",
		.mode = S_IFDIR | 0555,
	},
	[RPCAUTH_mount] = {
		.name = "mount",
		.mode = S_IFDIR | 0555,
	},
	[RPCAUTH_nfs] = {
		.name = "nfs",
		.mode = S_IFDIR | 0555,
	},
	[RPCAUTH_portmap] = {
		.name = "portmap",
		.mode = S_IFDIR | 0555,
	},
	[RPCAUTH_statd] = {
		.name = "statd",
		.mode = S_IFDIR | 0555,
	},
	[RPCAUTH_nfsd4_cb] = {
		.name = "nfsd4_cb",
		.mode = S_IFDIR | 0555,
	},
	[RPCAUTH_cache] = {
		.name = "cache",
		.mode = S_IFDIR | 0555,
	},
	[RPCAUTH_nfsd] = {
		.name = "nfsd",
		.mode = S_IFDIR | 0555,
	},
};

/*
 * This call can be used only in RPC pipefs mount notification hooks.
 */
struct dentry *rpc_d_lookup_sb(const struct super_block *sb,
			       const unsigned char *dir_name)
{
	return try_lookup_noperm(&QSTR(dir_name), sb->s_root);
}
EXPORT_SYMBOL_GPL(rpc_d_lookup_sb);

int rpc_pipefs_init_net(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

	sn->gssd_dummy = rpc_mkpipe_data(&gssd_dummy_pipe_ops, 0);
	if (IS_ERR(sn->gssd_dummy))
		return PTR_ERR(sn->gssd_dummy);

	mutex_init(&sn->pipefs_sb_lock);
	sn->pipe_version = -1;
	return 0;
}

void rpc_pipefs_exit_net(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

	rpc_destroy_pipe_data(sn->gssd_dummy);
}

/*
 * This call will be used for per network namespace operations calls.
 * Note: Function will be returned with pipefs_sb_lock taken if superblock was
 * found. This lock have to be released by rpc_put_sb_net() when all operations
 * will be completed.
 */
struct super_block *rpc_get_sb_net(const struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

	mutex_lock(&sn->pipefs_sb_lock);
	if (sn->pipefs_sb)
		return sn->pipefs_sb;
	mutex_unlock(&sn->pipefs_sb_lock);
	return NULL;
}
EXPORT_SYMBOL_GPL(rpc_get_sb_net);

void rpc_put_sb_net(const struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

	WARN_ON(sn->pipefs_sb == NULL);
	mutex_unlock(&sn->pipefs_sb_lock);
}
EXPORT_SYMBOL_GPL(rpc_put_sb_net);

static ssize_t
dummy_downcall(struct file *filp, const char __user *src, size_t len)
{
	return -EINVAL;
}

static const struct rpc_pipe_ops gssd_dummy_pipe_ops = {
	.upcall		= rpc_pipe_generic_upcall,
	.downcall	= dummy_downcall,
};

/*
 * Here we present a bogus "info" file to keep rpc.gssd happy. We don't expect
 * that it will ever use this info to handle an upcall, but rpc.gssd expects
 * that this file will be there and have a certain format.
 */
static int
rpc_dummy_info_show(struct seq_file *m, void *v)
{
	seq_printf(m, "RPC server: %s\n", utsname()->nodename);
	seq_printf(m, "service: foo (1) version 0\n");
	seq_printf(m, "address: 127.0.0.1\n");
	seq_printf(m, "protocol: tcp\n");
	seq_printf(m, "port: 0\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rpc_dummy_info);

/**
 * rpc_gssd_dummy_populate - create a dummy gssd pipe
 * @root:	root of the rpc_pipefs filesystem
 * @pipe_data:	pipe data created when netns is initialized
 *
 * Create a dummy set of directories and a pipe that gssd can hold open to
 * indicate that it is up and running.
 */
static int
rpc_gssd_dummy_populate(struct dentry *root, struct rpc_pipe *pipe_data)
{
	struct dentry *gssd_dentry, *clnt_dentry;
	int err;

	gssd_dentry = rpc_new_dir(root, "gssd", 0555);
	if (IS_ERR(gssd_dentry))
		return -ENOENT;

	clnt_dentry = rpc_new_dir(gssd_dentry, "clntXX", 0555);
	if (IS_ERR(clnt_dentry))
		return -ENOENT;

	err = rpc_new_file(clnt_dentry, "info", 0400,
				   &rpc_dummy_info_fops, NULL);
	if (!err)
		err = rpc_mkpipe_dentry(clnt_dentry, "gssd", NULL, pipe_data);
	return err;
}

static int
rpc_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;
	struct dentry *root;
	struct net *net = sb->s_fs_info;
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	int err;

	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = RPCAUTH_GSSMAGIC;
	sb->s_op = &s_ops;
	sb->s_d_flags = DCACHE_DONTCACHE;
	sb->s_time_gran = 1;

	inode = rpc_get_inode(sb, S_IFDIR | 0555);
	sb->s_root = root = d_make_root(inode);
	if (!root)
		return -ENOMEM;
	if (rpc_populate(root, files, RPCAUTH_lockd, RPCAUTH_RootEOF, NULL))
		return -ENOMEM;

	err = rpc_gssd_dummy_populate(root, sn->gssd_dummy);
	if (err)
		return err;

	dprintk("RPC:       sending pipefs MOUNT notification for net %x%s\n",
		net->ns.inum, NET_NAME(net));
	mutex_lock(&sn->pipefs_sb_lock);
	sn->pipefs_sb = sb;
	err = blocking_notifier_call_chain(&rpc_pipefs_notifier_list,
					   RPC_PIPEFS_MOUNT,
					   sb);
	mutex_unlock(&sn->pipefs_sb_lock);
	return err;
}

bool
gssd_running(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct rpc_pipe *pipe = sn->gssd_dummy;

	return pipe->nreaders || pipe->nwriters;
}
EXPORT_SYMBOL_GPL(gssd_running);

static int rpc_fs_get_tree(struct fs_context *fc)
{
	return get_tree_keyed(fc, rpc_fill_super, get_net(fc->net_ns));
}

static void rpc_fs_free_fc(struct fs_context *fc)
{
	if (fc->s_fs_info)
		put_net(fc->s_fs_info);
}

static const struct fs_context_operations rpc_fs_context_ops = {
	.free		= rpc_fs_free_fc,
	.get_tree	= rpc_fs_get_tree,
};

static int rpc_init_fs_context(struct fs_context *fc)
{
	put_user_ns(fc->user_ns);
	fc->user_ns = get_user_ns(fc->net_ns->user_ns);
	fc->ops = &rpc_fs_context_ops;
	return 0;
}

static void rpc_kill_sb(struct super_block *sb)
{
	struct net *net = sb->s_fs_info;
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

	mutex_lock(&sn->pipefs_sb_lock);
	if (sn->pipefs_sb != sb) {
		mutex_unlock(&sn->pipefs_sb_lock);
		goto out;
	}
	sn->pipefs_sb = NULL;
	dprintk("RPC:       sending pipefs UMOUNT notification for net %x%s\n",
		net->ns.inum, NET_NAME(net));
	blocking_notifier_call_chain(&rpc_pipefs_notifier_list,
					   RPC_PIPEFS_UMOUNT,
					   sb);
	mutex_unlock(&sn->pipefs_sb_lock);
out:
	kill_litter_super(sb);
	put_net(net);
}

static struct file_system_type rpc_pipe_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "rpc_pipefs",
	.init_fs_context = rpc_init_fs_context,
	.kill_sb	= rpc_kill_sb,
};
MODULE_ALIAS_FS("rpc_pipefs");
MODULE_ALIAS("rpc_pipefs");

static void
init_once(void *foo)
{
	struct rpc_inode *rpci = (struct rpc_inode *) foo;

	inode_init_once(&rpci->vfs_inode);
	rpci->private = NULL;
	rpci->pipe = NULL;
	init_waitqueue_head(&rpci->waitq);
}

int register_rpc_pipefs(void)
{
	int err;

	rpc_inode_cachep = kmem_cache_create("rpc_inode_cache",
				sizeof(struct rpc_inode),
				0, (SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT|
						SLAB_ACCOUNT),
				init_once);
	if (!rpc_inode_cachep)
		return -ENOMEM;
	err = rpc_clients_notifier_register();
	if (err)
		goto err_notifier;
	err = register_filesystem(&rpc_pipe_fs_type);
	if (err)
		goto err_register;
	return 0;

err_register:
	rpc_clients_notifier_unregister();
err_notifier:
	kmem_cache_destroy(rpc_inode_cachep);
	return err;
}

void unregister_rpc_pipefs(void)
{
	rpc_clients_notifier_unregister();
	unregister_filesystem(&rpc_pipe_fs_type);
	kmem_cache_destroy(rpc_inode_cachep);
}
