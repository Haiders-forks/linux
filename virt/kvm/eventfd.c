// SPDX-License-Identifier: GPL-2.0-only
/*
 * kvm eventfd support - use eventfd objects to signal various KVM events
 *
 * Copyright 2009 Novell.  All Rights Reserved.
 * Copyright 2010 Red Hat, Inc. and/or its affiliates.
 *
 * Author:
 *	Gregory Haskins <ghaskins@novell.com>
 */

#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include <linux/kvm_irqfd.h>
#include <linux/workqueue.h>
#include <linux/syscalls.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/eventfd.h>
#include <linux/kernel.h>
#include <linux/srcu.h>
#include <linux/slab.h>
#include <linux/seqlock.h>
#include <linux/irqbypass.h>
#include <trace/events/kvm.h>

#include <kvm/iodev.h>

#ifdef CONFIG_HAVE_KVM_IRQCHIP

static struct workqueue_struct *irqfd_cleanup_wq;

bool __attribute__((weak))
kvm_arch_irqfd_allowed(struct kvm *kvm, struct kvm_irqfd *args)
{
	return true;
}

static void
irqfd_inject(struct work_struct *work)
{
	struct kvm_kernel_irqfd *irqfd =
		container_of(work, struct kvm_kernel_irqfd, inject);
	struct kvm *kvm = irqfd->kvm;

	if (!irqfd->resampler) {
		kvm_set_irq(kvm, KVM_USERSPACE_IRQ_SOURCE_ID, irqfd->gsi, 1,
				false);
		kvm_set_irq(kvm, KVM_USERSPACE_IRQ_SOURCE_ID, irqfd->gsi, 0,
				false);
	} else
		kvm_set_irq(kvm, KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID,
			    irqfd->gsi, 1, false);
}

static void irqfd_resampler_notify(struct kvm_kernel_irqfd_resampler *resampler)
{
	struct kvm_kernel_irqfd *irqfd;

	list_for_each_entry_srcu(irqfd, &resampler->list, resampler_link,
				 srcu_read_lock_held(&resampler->kvm->irq_srcu))
		eventfd_signal(irqfd->resamplefd);
}

/*
 * Since resampler irqfds share an IRQ source ID, we de-assert once
 * then notify all of the resampler irqfds using this GSI.  We can't
 * do multiple de-asserts or we risk racing with incoming re-asserts.
 */
static void
irqfd_resampler_ack(struct kvm_irq_ack_notifier *kian)
{
	struct kvm_kernel_irqfd_resampler *resampler;
	struct kvm *kvm;
	int idx;

	resampler = container_of(kian,
			struct kvm_kernel_irqfd_resampler, notifier);
	kvm = resampler->kvm;

	kvm_set_irq(kvm, KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID,
		    resampler->notifier.gsi, 0, false);

	idx = srcu_read_lock(&kvm->irq_srcu);
	irqfd_resampler_notify(resampler);
	srcu_read_unlock(&kvm->irq_srcu, idx);
}

static void
irqfd_resampler_shutdown(struct kvm_kernel_irqfd *irqfd)
{
	struct kvm_kernel_irqfd_resampler *resampler = irqfd->resampler;
	struct kvm *kvm = resampler->kvm;

	mutex_lock(&kvm->irqfds.resampler_lock);

	list_del_rcu(&irqfd->resampler_link);

	if (list_empty(&resampler->list)) {
		list_del_rcu(&resampler->link);
		kvm_unregister_irq_ack_notifier(kvm, &resampler->notifier);
		/*
		 * synchronize_srcu_expedited(&kvm->irq_srcu) already called
		 * in kvm_unregister_irq_ack_notifier().
		 */
		kvm_set_irq(kvm, KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID,
			    resampler->notifier.gsi, 0, false);
		kfree(resampler);
	} else {
		synchronize_srcu_expedited(&kvm->irq_srcu);
	}

	mutex_unlock(&kvm->irqfds.resampler_lock);
}

/*
 * Race-free decouple logic (ordering is critical)
 */
static void
irqfd_shutdown(struct work_struct *work)
{
	struct kvm_kernel_irqfd *irqfd =
		container_of(work, struct kvm_kernel_irqfd, shutdown);
	struct kvm *kvm = irqfd->kvm;
	u64 cnt;

	/* Make sure irqfd has been initialized in assign path. */
	synchronize_srcu_expedited(&kvm->irq_srcu);

	/*
	 * Synchronize with the wait-queue and unhook ourselves to prevent
	 * further events.
	 */
	eventfd_ctx_remove_wait_queue(irqfd->eventfd, &irqfd->wait, &cnt);

	/*
	 * We know no new events will be scheduled at this point, so block
	 * until all previously outstanding events have completed
	 */
	flush_work(&irqfd->inject);

	if (irqfd->resampler) {
		irqfd_resampler_shutdown(irqfd);
		eventfd_ctx_put(irqfd->resamplefd);
	}

	/*
	 * It is now safe to release the object's resources
	 */
#if IS_ENABLED(CONFIG_HAVE_KVM_IRQ_BYPASS)
	irq_bypass_unregister_consumer(&irqfd->consumer);
#endif
	eventfd_ctx_put(irqfd->eventfd);
	kfree(irqfd);
}


/* assumes kvm->irqfds.lock is held */
static bool
irqfd_is_active(struct kvm_kernel_irqfd *irqfd)
{
	return list_empty(&irqfd->list) ? false : true;
}

/*
 * Mark the irqfd as inactive and schedule it for removal
 *
 * assumes kvm->irqfds.lock is held
 */
static void
irqfd_deactivate(struct kvm_kernel_irqfd *irqfd)
{
	BUG_ON(!irqfd_is_active(irqfd));

	list_del_init(&irqfd->list);

	queue_work(irqfd_cleanup_wq, &irqfd->shutdown);
}

int __attribute__((weak)) kvm_arch_set_irq_inatomic(
				struct kvm_kernel_irq_routing_entry *irq,
				struct kvm *kvm, int irq_source_id,
				int level,
				bool line_status)
{
	return -EWOULDBLOCK;
}

/*
 * Called with wqh->lock held and interrupts disabled
 */
static int
irqfd_wakeup(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
	struct kvm_kernel_irqfd *irqfd =
		container_of(wait, struct kvm_kernel_irqfd, wait);
	__poll_t flags = key_to_poll(key);
	struct kvm_kernel_irq_routing_entry irq;
	struct kvm *kvm = irqfd->kvm;
	unsigned seq;
	int idx;
	int ret = 0;

	if (flags & EPOLLIN) {
		/*
		 * WARNING: Do NOT take irqfds.lock in any path except EPOLLHUP,
		 * as KVM holds irqfds.lock when registering the irqfd with the
		 * eventfd.
		 */
		u64 cnt;
		eventfd_ctx_do_read(irqfd->eventfd, &cnt);

		idx = srcu_read_lock(&kvm->irq_srcu);
		do {
			seq = read_seqcount_begin(&irqfd->irq_entry_sc);
			irq = irqfd->irq_entry;
		} while (read_seqcount_retry(&irqfd->irq_entry_sc, seq));
		/* An event has been signaled, inject an interrupt */
		if (kvm_arch_set_irq_inatomic(&irq, kvm,
					      KVM_USERSPACE_IRQ_SOURCE_ID, 1,
					      false) == -EWOULDBLOCK)
			schedule_work(&irqfd->inject);
		srcu_read_unlock(&kvm->irq_srcu, idx);
		ret = 1;
	}

	if (flags & EPOLLHUP) {
		/* The eventfd is closing, detach from KVM */
		unsigned long iflags;

		/*
		 * Taking irqfds.lock is safe here, as KVM holds a reference to
		 * the eventfd when registering the irqfd, i.e. this path can't
		 * be reached while kvm_irqfd_add() is running.
		 */
		spin_lock_irqsave(&kvm->irqfds.lock, iflags);

		/*
		 * We must check if someone deactivated the irqfd before
		 * we could acquire the irqfds.lock since the item is
		 * deactivated from the KVM side before it is unhooked from
		 * the wait-queue.  If it is already deactivated, we can
		 * simply return knowing the other side will cleanup for us.
		 * We cannot race against the irqfd going away since the
		 * other side is required to acquire wqh->lock, which we hold
		 */
		if (irqfd_is_active(irqfd))
			irqfd_deactivate(irqfd);

		spin_unlock_irqrestore(&kvm->irqfds.lock, iflags);
	}

	return ret;
}

static void irqfd_update(struct kvm *kvm, struct kvm_kernel_irqfd *irqfd)
{
	struct kvm_kernel_irq_routing_entry *e;
	struct kvm_kernel_irq_routing_entry entries[KVM_NR_IRQCHIPS];
	int n_entries;

	lockdep_assert_held(&kvm->irqfds.lock);

	n_entries = kvm_irq_map_gsi(kvm, entries, irqfd->gsi);

	write_seqcount_begin(&irqfd->irq_entry_sc);

	e = entries;
	if (n_entries == 1)
		irqfd->irq_entry = *e;
	else
		irqfd->irq_entry.type = 0;

	write_seqcount_end(&irqfd->irq_entry_sc);
}

struct kvm_irqfd_pt {
	struct kvm_kernel_irqfd *irqfd;
	struct kvm *kvm;
	poll_table pt;
	int ret;
};

static void kvm_irqfd_register(struct file *file, wait_queue_head_t *wqh,
			       poll_table *pt)
{
	struct kvm_irqfd_pt *p = container_of(pt, struct kvm_irqfd_pt, pt);
	struct kvm_kernel_irqfd *irqfd = p->irqfd;
	struct kvm *kvm = p->kvm;

	/*
	 * Note, irqfds.lock protects the irqfd's irq_entry, i.e. its routing,
	 * and irqfds.items.  It does NOT protect registering with the eventfd.
	 */
	spin_lock_irq(&kvm->irqfds.lock);

	/*
	 * Initialize the routing information prior to adding the irqfd to the
	 * eventfd's waitqueue, as irqfd_wakeup() can be invoked as soon as the
	 * irqfd is registered.
	 */
	irqfd_update(kvm, irqfd);

	/*
	 * Add the irqfd as a priority waiter on the eventfd, with a custom
	 * wake-up handler, so that KVM *and only KVM* is notified whenever the
	 * underlying eventfd is signaled.
	 */
	init_waitqueue_func_entry(&irqfd->wait, irqfd_wakeup);

	/*
	 * Temporarily lie to lockdep about holding irqfds.lock to avoid a
	 * false positive regarding potential deadlock with irqfd_wakeup()
	 * (see irqfd_wakeup() for details).
	 *
	 * Adding to the wait queue will fail if there is already a priority
	 * waiter, i.e. if the eventfd is associated with another irqfd (in any
	 * VM).  Note, kvm_irqfd_deassign() waits for all in-flight shutdown
	 * jobs to complete, i.e. ensures the irqfd has been removed from the
	 * eventfd's waitqueue before returning to userspace.
	 */
	spin_release(&kvm->irqfds.lock.dep_map, _RET_IP_);
	p->ret = add_wait_queue_priority_exclusive(wqh, &irqfd->wait);
	spin_acquire(&kvm->irqfds.lock.dep_map, 0, 0, _RET_IP_);
	if (p->ret)
		goto out;

	list_add_tail(&irqfd->list, &kvm->irqfds.items);

out:
	spin_unlock_irq(&kvm->irqfds.lock);
}

#if IS_ENABLED(CONFIG_HAVE_KVM_IRQ_BYPASS)
void __attribute__((weak)) kvm_arch_irq_bypass_stop(
				struct irq_bypass_consumer *cons)
{
}

void __attribute__((weak)) kvm_arch_irq_bypass_start(
				struct irq_bypass_consumer *cons)
{
}

void __weak kvm_arch_update_irqfd_routing(struct kvm_kernel_irqfd *irqfd,
					  struct kvm_kernel_irq_routing_entry *old,
					  struct kvm_kernel_irq_routing_entry *new)
{

}
#endif

static int
kvm_irqfd_assign(struct kvm *kvm, struct kvm_irqfd *args)
{
	struct kvm_kernel_irqfd *irqfd;
	struct eventfd_ctx *eventfd = NULL, *resamplefd = NULL;
	struct kvm_irqfd_pt irqfd_pt;
	int ret;
	__poll_t events;
	int idx;

	if (!kvm_arch_intc_initialized(kvm))
		return -EAGAIN;

	if (!kvm_arch_irqfd_allowed(kvm, args))
		return -EINVAL;

	irqfd = kzalloc(sizeof(*irqfd), GFP_KERNEL_ACCOUNT);
	if (!irqfd)
		return -ENOMEM;

	irqfd->kvm = kvm;
	irqfd->gsi = args->gsi;
	INIT_LIST_HEAD(&irqfd->list);
	INIT_WORK(&irqfd->inject, irqfd_inject);
	INIT_WORK(&irqfd->shutdown, irqfd_shutdown);
	seqcount_spinlock_init(&irqfd->irq_entry_sc, &kvm->irqfds.lock);

	CLASS(fd, f)(args->fd);
	if (fd_empty(f)) {
		ret = -EBADF;
		goto out;
	}

	eventfd = eventfd_ctx_fileget(fd_file(f));
	if (IS_ERR(eventfd)) {
		ret = PTR_ERR(eventfd);
		goto out;
	}

	irqfd->eventfd = eventfd;

	if (args->flags & KVM_IRQFD_FLAG_RESAMPLE) {
		struct kvm_kernel_irqfd_resampler *resampler;

		resamplefd = eventfd_ctx_fdget(args->resamplefd);
		if (IS_ERR(resamplefd)) {
			ret = PTR_ERR(resamplefd);
			goto fail;
		}

		irqfd->resamplefd = resamplefd;
		INIT_LIST_HEAD(&irqfd->resampler_link);

		mutex_lock(&kvm->irqfds.resampler_lock);

		list_for_each_entry(resampler,
				    &kvm->irqfds.resampler_list, link) {
			if (resampler->notifier.gsi == irqfd->gsi) {
				irqfd->resampler = resampler;
				break;
			}
		}

		if (!irqfd->resampler) {
			resampler = kzalloc(sizeof(*resampler),
					    GFP_KERNEL_ACCOUNT);
			if (!resampler) {
				ret = -ENOMEM;
				mutex_unlock(&kvm->irqfds.resampler_lock);
				goto fail;
			}

			resampler->kvm = kvm;
			INIT_LIST_HEAD(&resampler->list);
			resampler->notifier.gsi = irqfd->gsi;
			resampler->notifier.irq_acked = irqfd_resampler_ack;
			INIT_LIST_HEAD(&resampler->link);

			list_add_rcu(&resampler->link, &kvm->irqfds.resampler_list);
			kvm_register_irq_ack_notifier(kvm,
						      &resampler->notifier);
			irqfd->resampler = resampler;
		}

		list_add_rcu(&irqfd->resampler_link, &irqfd->resampler->list);
		synchronize_srcu_expedited(&kvm->irq_srcu);

		mutex_unlock(&kvm->irqfds.resampler_lock);
	}

	/*
	 * Set the irqfd routing and add it to KVM's list before registering
	 * the irqfd with the eventfd, so that the routing information is valid
	 * and stays valid, e.g. if there are GSI routing changes, prior to
	 * making the irqfd visible, i.e. before it might be signaled.
	 *
	 * Note, holding SRCU ensures a stable read of routing information, and
	 * also prevents irqfd_shutdown() from freeing the irqfd before it's
	 * fully initialized.
	 */
	idx = srcu_read_lock(&kvm->irq_srcu);

	/*
	 * Register the irqfd with the eventfd by polling on the eventfd, and
	 * simultaneously and the irqfd to KVM's list.  If there was en event
	 * pending on the eventfd prior to registering, manually trigger IRQ
	 * injection.
	 */
	irqfd_pt.irqfd = irqfd;
	irqfd_pt.kvm = kvm;
	init_poll_funcptr(&irqfd_pt.pt, kvm_irqfd_register);

	events = vfs_poll(fd_file(f), &irqfd_pt.pt);

	ret = irqfd_pt.ret;
	if (ret)
		goto fail_poll;

	if (events & EPOLLIN)
		schedule_work(&irqfd->inject);

#if IS_ENABLED(CONFIG_HAVE_KVM_IRQ_BYPASS)
	if (kvm_arch_has_irq_bypass()) {
		irqfd->consumer.add_producer = kvm_arch_irq_bypass_add_producer;
		irqfd->consumer.del_producer = kvm_arch_irq_bypass_del_producer;
		irqfd->consumer.stop = kvm_arch_irq_bypass_stop;
		irqfd->consumer.start = kvm_arch_irq_bypass_start;
		ret = irq_bypass_register_consumer(&irqfd->consumer, irqfd->eventfd);
		if (ret)
			pr_info("irq bypass consumer (eventfd %p) registration fails: %d\n",
				irqfd->eventfd, ret);
	}
#endif

	srcu_read_unlock(&kvm->irq_srcu, idx);
	return 0;

fail_poll:
	srcu_read_unlock(&kvm->irq_srcu, idx);
fail:
	if (irqfd->resampler)
		irqfd_resampler_shutdown(irqfd);

	if (resamplefd && !IS_ERR(resamplefd))
		eventfd_ctx_put(resamplefd);

	if (eventfd && !IS_ERR(eventfd))
		eventfd_ctx_put(eventfd);

out:
	kfree(irqfd);
	return ret;
}

bool kvm_irq_has_notifier(struct kvm *kvm, unsigned irqchip, unsigned pin)
{
	struct kvm_irq_ack_notifier *kian;
	int gsi, idx;

	idx = srcu_read_lock(&kvm->irq_srcu);
	gsi = kvm_irq_map_chip_pin(kvm, irqchip, pin);
	if (gsi != -1)
		hlist_for_each_entry_srcu(kian, &kvm->irq_ack_notifier_list,
					  link, srcu_read_lock_held(&kvm->irq_srcu))
			if (kian->gsi == gsi) {
				srcu_read_unlock(&kvm->irq_srcu, idx);
				return true;
			}

	srcu_read_unlock(&kvm->irq_srcu, idx);

	return false;
}
EXPORT_SYMBOL_GPL(kvm_irq_has_notifier);

void kvm_notify_acked_gsi(struct kvm *kvm, int gsi)
{
	struct kvm_irq_ack_notifier *kian;

	hlist_for_each_entry_srcu(kian, &kvm->irq_ack_notifier_list,
				  link, srcu_read_lock_held(&kvm->irq_srcu))
		if (kian->gsi == gsi)
			kian->irq_acked(kian);
}

void kvm_notify_acked_irq(struct kvm *kvm, unsigned irqchip, unsigned pin)
{
	int gsi, idx;

	trace_kvm_ack_irq(irqchip, pin);

	idx = srcu_read_lock(&kvm->irq_srcu);
	gsi = kvm_irq_map_chip_pin(kvm, irqchip, pin);
	if (gsi != -1)
		kvm_notify_acked_gsi(kvm, gsi);
	srcu_read_unlock(&kvm->irq_srcu, idx);
}

void kvm_register_irq_ack_notifier(struct kvm *kvm,
				   struct kvm_irq_ack_notifier *kian)
{
	mutex_lock(&kvm->irq_lock);
	hlist_add_head_rcu(&kian->link, &kvm->irq_ack_notifier_list);
	mutex_unlock(&kvm->irq_lock);
	kvm_arch_post_irq_ack_notifier_list_update(kvm);
}

void kvm_unregister_irq_ack_notifier(struct kvm *kvm,
				    struct kvm_irq_ack_notifier *kian)
{
	mutex_lock(&kvm->irq_lock);
	hlist_del_init_rcu(&kian->link);
	mutex_unlock(&kvm->irq_lock);
	synchronize_srcu_expedited(&kvm->irq_srcu);
	kvm_arch_post_irq_ack_notifier_list_update(kvm);
}

/*
 * shutdown any irqfd's that match fd+gsi
 */
static int
kvm_irqfd_deassign(struct kvm *kvm, struct kvm_irqfd *args)
{
	struct kvm_kernel_irqfd *irqfd, *tmp;
	struct eventfd_ctx *eventfd;

	eventfd = eventfd_ctx_fdget(args->fd);
	if (IS_ERR(eventfd))
		return PTR_ERR(eventfd);

	spin_lock_irq(&kvm->irqfds.lock);

	list_for_each_entry_safe(irqfd, tmp, &kvm->irqfds.items, list) {
		if (irqfd->eventfd == eventfd && irqfd->gsi == args->gsi) {
			/*
			 * This clearing of irq_entry.type is needed for when
			 * another thread calls kvm_irq_routing_update before
			 * we flush workqueue below (we synchronize with
			 * kvm_irq_routing_update using irqfds.lock).
			 */
			write_seqcount_begin(&irqfd->irq_entry_sc);
			irqfd->irq_entry.type = 0;
			write_seqcount_end(&irqfd->irq_entry_sc);
			irqfd_deactivate(irqfd);
		}
	}

	spin_unlock_irq(&kvm->irqfds.lock);
	eventfd_ctx_put(eventfd);

	/*
	 * Block until we know all outstanding shutdown jobs have completed
	 * so that we guarantee there will not be any more interrupts on this
	 * gsi once this deassign function returns.
	 */
	flush_workqueue(irqfd_cleanup_wq);

	return 0;
}

int
kvm_irqfd(struct kvm *kvm, struct kvm_irqfd *args)
{
	if (args->flags & ~(KVM_IRQFD_FLAG_DEASSIGN | KVM_IRQFD_FLAG_RESAMPLE))
		return -EINVAL;

	if (args->flags & KVM_IRQFD_FLAG_DEASSIGN)
		return kvm_irqfd_deassign(kvm, args);

	return kvm_irqfd_assign(kvm, args);
}

/*
 * This function is called as the kvm VM fd is being released. Shutdown all
 * irqfds that still remain open
 */
void
kvm_irqfd_release(struct kvm *kvm)
{
	struct kvm_kernel_irqfd *irqfd, *tmp;

	spin_lock_irq(&kvm->irqfds.lock);

	list_for_each_entry_safe(irqfd, tmp, &kvm->irqfds.items, list)
		irqfd_deactivate(irqfd);

	spin_unlock_irq(&kvm->irqfds.lock);

	/*
	 * Block until we know all outstanding shutdown jobs have completed
	 * since we do not take a kvm* reference.
	 */
	flush_workqueue(irqfd_cleanup_wq);

}

/*
 * Take note of a change in irq routing.
 * Caller must invoke synchronize_srcu_expedited(&kvm->irq_srcu) afterwards.
 */
void kvm_irq_routing_update(struct kvm *kvm)
{
	struct kvm_kernel_irqfd *irqfd;

	spin_lock_irq(&kvm->irqfds.lock);

	list_for_each_entry(irqfd, &kvm->irqfds.items, list) {
#if IS_ENABLED(CONFIG_HAVE_KVM_IRQ_BYPASS)
		/* Under irqfds.lock, so can read irq_entry safely */
		struct kvm_kernel_irq_routing_entry old = irqfd->irq_entry;
#endif

		irqfd_update(kvm, irqfd);

#if IS_ENABLED(CONFIG_HAVE_KVM_IRQ_BYPASS)
		if (irqfd->producer)
			kvm_arch_update_irqfd_routing(irqfd, &old, &irqfd->irq_entry);
#endif
	}

	spin_unlock_irq(&kvm->irqfds.lock);
}

bool kvm_notify_irqfd_resampler(struct kvm *kvm,
				unsigned int irqchip,
				unsigned int pin)
{
	struct kvm_kernel_irqfd_resampler *resampler;
	int gsi, idx;

	idx = srcu_read_lock(&kvm->irq_srcu);
	gsi = kvm_irq_map_chip_pin(kvm, irqchip, pin);
	if (gsi != -1) {
		list_for_each_entry_srcu(resampler,
					 &kvm->irqfds.resampler_list, link,
					 srcu_read_lock_held(&kvm->irq_srcu)) {
			if (resampler->notifier.gsi == gsi) {
				irqfd_resampler_notify(resampler);
				srcu_read_unlock(&kvm->irq_srcu, idx);
				return true;
			}
		}
	}
	srcu_read_unlock(&kvm->irq_srcu, idx);

	return false;
}

/*
 * create a host-wide workqueue for issuing deferred shutdown requests
 * aggregated from all vm* instances. We need our own isolated
 * queue to ease flushing work items when a VM exits.
 */
int kvm_irqfd_init(void)
{
	irqfd_cleanup_wq = alloc_workqueue("kvm-irqfd-cleanup", 0, 0);
	if (!irqfd_cleanup_wq)
		return -ENOMEM;

	return 0;
}

void kvm_irqfd_exit(void)
{
	destroy_workqueue(irqfd_cleanup_wq);
}
#endif

/*
 * --------------------------------------------------------------------
 * ioeventfd: translate a PIO/MMIO memory write to an eventfd signal.
 *
 * userspace can register a PIO/MMIO address with an eventfd for receiving
 * notification when the memory has been touched.
 * --------------------------------------------------------------------
 */

struct _ioeventfd {
	struct list_head     list;
	u64                  addr;
	int                  length;
	struct eventfd_ctx  *eventfd;
	u64                  datamatch;
	struct kvm_io_device dev;
	u8                   bus_idx;
	bool                 wildcard;
};

static inline struct _ioeventfd *
to_ioeventfd(struct kvm_io_device *dev)
{
	return container_of(dev, struct _ioeventfd, dev);
}

static void
ioeventfd_release(struct _ioeventfd *p)
{
	eventfd_ctx_put(p->eventfd);
	list_del(&p->list);
	kfree(p);
}

static bool
ioeventfd_in_range(struct _ioeventfd *p, gpa_t addr, int len, const void *val)
{
	u64 _val;

	if (addr != p->addr)
		/* address must be precise for a hit */
		return false;

	if (!p->length)
		/* length = 0 means only look at the address, so always a hit */
		return true;

	if (len != p->length)
		/* address-range must be precise for a hit */
		return false;

	if (p->wildcard)
		/* all else equal, wildcard is always a hit */
		return true;

	/* otherwise, we have to actually compare the data */

	BUG_ON(!IS_ALIGNED((unsigned long)val, len));

	switch (len) {
	case 1:
		_val = *(u8 *)val;
		break;
	case 2:
		_val = *(u16 *)val;
		break;
	case 4:
		_val = *(u32 *)val;
		break;
	case 8:
		_val = *(u64 *)val;
		break;
	default:
		return false;
	}

	return _val == p->datamatch;
}

/* MMIO/PIO writes trigger an event if the addr/val match */
static int
ioeventfd_write(struct kvm_vcpu *vcpu, struct kvm_io_device *this, gpa_t addr,
		int len, const void *val)
{
	struct _ioeventfd *p = to_ioeventfd(this);

	if (!ioeventfd_in_range(p, addr, len, val))
		return -EOPNOTSUPP;

	eventfd_signal(p->eventfd);
	return 0;
}

/*
 * This function is called as KVM is completely shutting down.  We do not
 * need to worry about locking just nuke anything we have as quickly as possible
 */
static void
ioeventfd_destructor(struct kvm_io_device *this)
{
	struct _ioeventfd *p = to_ioeventfd(this);

	ioeventfd_release(p);
}

static const struct kvm_io_device_ops ioeventfd_ops = {
	.write      = ioeventfd_write,
	.destructor = ioeventfd_destructor,
};

/* assumes kvm->slots_lock held */
static bool
ioeventfd_check_collision(struct kvm *kvm, struct _ioeventfd *p)
{
	struct _ioeventfd *_p;

	list_for_each_entry(_p, &kvm->ioeventfds, list)
		if (_p->bus_idx == p->bus_idx &&
		    _p->addr == p->addr &&
		    (!_p->length || !p->length ||
		     (_p->length == p->length &&
		      (_p->wildcard || p->wildcard ||
		       _p->datamatch == p->datamatch))))
			return true;

	return false;
}

static enum kvm_bus ioeventfd_bus_from_flags(__u32 flags)
{
	if (flags & KVM_IOEVENTFD_FLAG_PIO)
		return KVM_PIO_BUS;
	if (flags & KVM_IOEVENTFD_FLAG_VIRTIO_CCW_NOTIFY)
		return KVM_VIRTIO_CCW_NOTIFY_BUS;
	return KVM_MMIO_BUS;
}

static int kvm_assign_ioeventfd_idx(struct kvm *kvm,
				enum kvm_bus bus_idx,
				struct kvm_ioeventfd *args)
{

	struct eventfd_ctx *eventfd;
	struct _ioeventfd *p;
	int ret;

	eventfd = eventfd_ctx_fdget(args->fd);
	if (IS_ERR(eventfd))
		return PTR_ERR(eventfd);

	p = kzalloc(sizeof(*p), GFP_KERNEL_ACCOUNT);
	if (!p) {
		ret = -ENOMEM;
		goto fail;
	}

	INIT_LIST_HEAD(&p->list);
	p->addr    = args->addr;
	p->bus_idx = bus_idx;
	p->length  = args->len;
	p->eventfd = eventfd;

	/* The datamatch feature is optional, otherwise this is a wildcard */
	if (args->flags & KVM_IOEVENTFD_FLAG_DATAMATCH)
		p->datamatch = args->datamatch;
	else
		p->wildcard = true;

	mutex_lock(&kvm->slots_lock);

	/* Verify that there isn't a match already */
	if (ioeventfd_check_collision(kvm, p)) {
		ret = -EEXIST;
		goto unlock_fail;
	}

	kvm_iodevice_init(&p->dev, &ioeventfd_ops);

	ret = kvm_io_bus_register_dev(kvm, bus_idx, p->addr, p->length,
				      &p->dev);
	if (ret < 0)
		goto unlock_fail;

	kvm_get_bus(kvm, bus_idx)->ioeventfd_count++;
	list_add_tail(&p->list, &kvm->ioeventfds);

	mutex_unlock(&kvm->slots_lock);

	return 0;

unlock_fail:
	mutex_unlock(&kvm->slots_lock);
	kfree(p);

fail:
	eventfd_ctx_put(eventfd);

	return ret;
}

static int
kvm_deassign_ioeventfd_idx(struct kvm *kvm, enum kvm_bus bus_idx,
			   struct kvm_ioeventfd *args)
{
	struct _ioeventfd        *p;
	struct eventfd_ctx       *eventfd;
	struct kvm_io_bus	 *bus;
	int                       ret = -ENOENT;
	bool                      wildcard;

	eventfd = eventfd_ctx_fdget(args->fd);
	if (IS_ERR(eventfd))
		return PTR_ERR(eventfd);

	wildcard = !(args->flags & KVM_IOEVENTFD_FLAG_DATAMATCH);

	mutex_lock(&kvm->slots_lock);

	list_for_each_entry(p, &kvm->ioeventfds, list) {
		if (p->bus_idx != bus_idx ||
		    p->eventfd != eventfd  ||
		    p->addr != args->addr  ||
		    p->length != args->len ||
		    p->wildcard != wildcard)
			continue;

		if (!p->wildcard && p->datamatch != args->datamatch)
			continue;

		kvm_io_bus_unregister_dev(kvm, bus_idx, &p->dev);
		bus = kvm_get_bus(kvm, bus_idx);
		if (bus)
			bus->ioeventfd_count--;
		ret = 0;
		break;
	}

	mutex_unlock(&kvm->slots_lock);

	eventfd_ctx_put(eventfd);

	return ret;
}

static int kvm_deassign_ioeventfd(struct kvm *kvm, struct kvm_ioeventfd *args)
{
	enum kvm_bus bus_idx = ioeventfd_bus_from_flags(args->flags);
	int ret = kvm_deassign_ioeventfd_idx(kvm, bus_idx, args);

	if (!args->len && bus_idx == KVM_MMIO_BUS)
		kvm_deassign_ioeventfd_idx(kvm, KVM_FAST_MMIO_BUS, args);

	return ret;
}

static int
kvm_assign_ioeventfd(struct kvm *kvm, struct kvm_ioeventfd *args)
{
	enum kvm_bus              bus_idx;
	int ret;

	bus_idx = ioeventfd_bus_from_flags(args->flags);
	/* must be natural-word sized, or 0 to ignore length */
	switch (args->len) {
	case 0:
	case 1:
	case 2:
	case 4:
	case 8:
		break;
	default:
		return -EINVAL;
	}

	/* check for range overflow */
	if (args->addr + args->len < args->addr)
		return -EINVAL;

	/* check for extra flags that we don't understand */
	if (args->flags & ~KVM_IOEVENTFD_VALID_FLAG_MASK)
		return -EINVAL;

	/* ioeventfd with no length can't be combined with DATAMATCH */
	if (!args->len && (args->flags & KVM_IOEVENTFD_FLAG_DATAMATCH))
		return -EINVAL;

	ret = kvm_assign_ioeventfd_idx(kvm, bus_idx, args);
	if (ret)
		goto fail;

	/* When length is ignored, MMIO is also put on a separate bus, for
	 * faster lookups.
	 */
	if (!args->len && bus_idx == KVM_MMIO_BUS) {
		ret = kvm_assign_ioeventfd_idx(kvm, KVM_FAST_MMIO_BUS, args);
		if (ret < 0)
			goto fast_fail;
	}

	return 0;

fast_fail:
	kvm_deassign_ioeventfd_idx(kvm, bus_idx, args);
fail:
	return ret;
}

int
kvm_ioeventfd(struct kvm *kvm, struct kvm_ioeventfd *args)
{
	if (args->flags & KVM_IOEVENTFD_FLAG_DEASSIGN)
		return kvm_deassign_ioeventfd(kvm, args);

	return kvm_assign_ioeventfd(kvm, args);
}

void
kvm_eventfd_init(struct kvm *kvm)
{
#ifdef CONFIG_HAVE_KVM_IRQCHIP
	spin_lock_init(&kvm->irqfds.lock);
	INIT_LIST_HEAD(&kvm->irqfds.items);
	INIT_LIST_HEAD(&kvm->irqfds.resampler_list);
	mutex_init(&kvm->irqfds.resampler_lock);
#endif
	INIT_LIST_HEAD(&kvm->ioeventfds);
}
