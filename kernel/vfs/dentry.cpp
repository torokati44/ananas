/*
 * Ananas dentry cache (heavily based on icache.c)
 *
 * A 'dentry' is a directory entry, and can be seen as the function f:
 * directory_inode x entry_name -> inode.
 *
 * We try to keep as much entries in memory as possible, only overwriting
 * them if we really need to.
 */
#include <ananas/types.h>
#include <ananas/vfs/core.h>
#include <ananas/vfs/dentry.h>
#include <ananas/error.h>
#include <ananas/init.h>
#include <ananas/mm.h>
#include <ananas/lock.h>
#include <ananas/schedule.h>
#include <ananas/trace.h>
#include <ananas/lib.h>
#include <ananas/kdb.h>
#include "options.h"

TRACE_SETUP;

namespace {

mutex_t dcache_mtx;
struct DENTRY_QUEUE	dcache_inuse;
struct DENTRY_QUEUE	dcache_free;

inline void dcache_lock()
{
	mutex_lock(&dcache_mtx);
}

inline void dcache_unlock()
{
	mutex_unlock(&dcache_mtx);
}

inline void dcache_assert_locked()
{
	mutex_assert(&dcache_mtx, MTX_LOCKED);
}

errorcode_t
dcache_init()
{
	mutex_init(&dcache_mtx, "dcache");
	LIST_INIT(&dcache_inuse);
	LIST_INIT(&dcache_free);

	/*
	 * Make an empty cache; we allocate one big pool and set up pointers to the
	 * items as necessary.
	 */
	{
		auto dentry = static_cast<struct DENTRY*>(kmalloc(DCACHE_ITEMS_PER_FS * sizeof(struct DENTRY)));
		memset(dentry, 0, DCACHE_ITEMS_PER_FS * sizeof(struct DENTRY));
		for (int i = 0; i < DCACHE_ITEMS_PER_FS; i++, dentry++)
			LIST_APPEND(&dcache_free, dentry);
	}

	return ananas_success();
}

struct DENTRY*
dcache_find_entry_to_use()
{
	dcache_assert_locked();

	if (!LIST_EMPTY(&dcache_free)) {
			struct DENTRY* d = LIST_HEAD(&dcache_free);
			LIST_POP_HEAD(&dcache_free);
			return d;
	}

	/*
	 * Our dcache is ordered from old-to-new, so we'll start at the back and
	 * take anything which has no refs and isn't a root dentry.
	 */
	LIST_FOREACH_REVERSE_SAFE(&dcache_inuse, d, struct DENTRY) {
		if (d->d_refcount == 0 && (d->d_flags & DENTRY_FLAG_ROOT) == 0) {
			// This dentry should be good to use - remove any backing inode it has,
			// as we will overwrite it
			if (d->d_inode != NULL) {
				vfs_deref_inode(d->d_inode);
				d->d_inode = nullptr;
			}

			LIST_REMOVE(&dcache_inuse, d);
			return d;
		}
	}

	return nullptr;
}


} // unnamed namespace

static void dentry_deref_locked(struct DENTRY* de);

struct DENTRY*
dcache_create_root_dentry(struct VFS_MOUNTED_FS* fs)
{
	dcache_lock();

	struct DENTRY* d = dcache_find_entry_to_use();
	KASSERT(d != nullptr, "out of dentries"); // XXX deal with this

	d->d_fs = fs;
	d->d_refcount = 1; /* filesystem itself */
	d->d_inode = NULL; /* supplied by the file system */
	d->d_flags = DENTRY_FLAG_ROOT;
	strcpy(d->d_entry, "/");
	LIST_PREPEND(&dcache_inuse, d);

	dcache_unlock();
	return d;
}

/*
 *
 * Attempts to look up a given entry for a parent dentry. Returns a referenced
 * dentry entry on success.
 *
 * The only way for this function to return NULL is that the lookup is
 * currently pending; this means the attempt to is be retried.
 *
 * Note that this function must be called with a referenced dentry to ensure it
 * will not go away. This ref is not touched by this function.
 */
struct DENTRY*
dcache_lookup(struct DENTRY* parent, const char* entry)
{
	TRACE(VFS, FUNC, "parent=%p, entry='%s'", parent, entry);

	dcache_lock();

	/*
	 * XXX This is just a simple linear search which attempts to avoid
	 * overhead by moving recent entries to the start.
	 */
	LIST_FOREACH(&dcache_inuse, d, struct DENTRY) {
		if (d->d_parent != parent || strcmp(d->d_entry, entry) != 0)
			continue;

		/*
		 * It's quite possible that this inode is still pending; if that is the
		 * case, our caller should sleep and wait for the other caller to finish
		 * up.
		 *
		 * XXX We shouldn't burden the caller with this!
		 */
		if (d->d_inode == nullptr && (d->d_flags & DENTRY_FLAG_NEGATIVE) == 0) {
			dcache_unlock();
			return nullptr;
		}

		// Add an extra ref to the dentry; we'll be giving it to the caller. Don't use dentry_ref()
		// here as the original refcount may be zero.
		++d->d_refcount;

		// Push the the item to the head of the cache
		LIST_REMOVE(&dcache_inuse, d);
		LIST_PREPEND(&dcache_inuse, d);
		dcache_unlock();
		TRACE(VFS, INFO, "cache hit: parent=%p, entry='%s' => d=%p, d.inode=%p", parent, entry, d, d->d_inode);
		return d;
	}

	// Item was not found; try to get one from the freelist
	struct DENTRY* d = nullptr;
	while(d == nullptr) {
		/* We are out of dcache entries; we should remove some of the older entries */
		d = dcache_find_entry_to_use();
		/* XXX we should be able to cope with the next condition, somehow */
		KASSERT(d != nullptr, "dcache full");
	}

	/* Add an explicit ref to the parent dentry; it will be referenced by our new dentry */
	dentry_ref(parent);

	/* Initialize the item */
	memset(d, 0, sizeof *d);
	d->d_fs = parent->d_fs;
	d->d_refcount = 1; // the caller
	d->d_parent = parent;
	d->d_inode = NULL;
	d->d_flags = 0;
	strcpy(d->d_entry, entry);
	LIST_PREPEND(&dcache_inuse, d);
	dcache_unlock();
	TRACE(VFS, INFO, "cache miss: parent=%p, entry='%s' => d=%p", parent, entry, d);
	return d;
}

void
dcache_purge_old_entries()
{
	dcache_lock();
	LIST_FOREACH_SAFE(&dcache_inuse, d, struct DENTRY) {
		if (d->d_refcount > 0 || (d->d_flags & DENTRY_FLAG_ROOT))
			continue; // in use or root, skip

		// Get rid of any backing inode; this is why we are called
		if (d->d_inode != NULL) {
			vfs_deref_inode(d->d_inode);
			d->d_inode = nullptr;
		}

		LIST_REMOVE(&dcache_inuse, d);
		LIST_PREPEND(&dcache_free, d);
	}
	dcache_unlock();
}

void
dcache_set_inode(struct DENTRY* de, struct VFS_INODE* inode)
{
#if 0
	/* XXX NOTYET - negative flag is cleared to keep the entry alive */
	KASSERT((de->d_flags & DENTRY_FLAG_NEGATIVE), "entry is not negative");
#endif
	KASSERT(inode != NULL, "no inode given");

	/* If we already have an inode, deref it; we don't care about it anymore */
	if (de->d_inode != NULL)
		vfs_deref_inode(de->d_inode);

	/* Increase the refcount - the cache will have a ref to the inode now */
	vfs_ref_inode(inode);
	de->d_inode = inode;
	de->d_flags &= ~DENTRY_FLAG_NEGATIVE;
}

void
dentry_ref(struct DENTRY* d)
{
	KASSERT(d->d_refcount > 0, "invalid refcount %d", d->d_refcount);
	d->d_refcount++;
}

static void
dentry_deref_locked(struct DENTRY* d)
{
	KASSERT(d->d_refcount > 0, "invalid refcount %d", d->d_refcount);

	// Remove a reference; if this brings us to zero, we need to remove it
	if (--d->d_refcount > 0)
		return;

	// We do not free backing inodes here - the reason is that we don't know
	// how they are to be re-looked up.

	// Free our reference to the parent
	if (d->d_parent != NULL) {
		dentry_deref_locked(d->d_parent);
		d->d_parent = nullptr;
	}
}

void
dentry_unlink(struct DENTRY* de)
{
	dcache_lock();
	de->d_flags |= DENTRY_FLAG_NEGATIVE;
	if (de->d_inode != nullptr)
		vfs_deref_inode(de->d_inode);
  de->d_inode = nullptr;
	dcache_unlock();
}

void
dentry_deref(struct DENTRY* de)
{
	dcache_lock();
	dentry_deref_locked(de);
	dcache_unlock();
}

void
dcache_purge()
{
	dcache_lock();
	dcache_unlock();
}

#ifdef OPTION_KDB
KDB_COMMAND(dcache, NULL, "Show dentry cache")
{
	/* XXX Don't lock; this is for debugging purposes only */
	int n = 0;
	LIST_FOREACH(&dcache_inuse, d, struct DENTRY) {
		kprintf("dcache_entry=%p, parent=%p, inode=%p, reverse name=%s[%d]",
		 d, d->d_parent, d->d_inode, d->d_entry, d->d_refcount);
		for (struct DENTRY* curde = d->d_parent; curde != NULL; curde = curde->d_parent)
			kprintf(",%s[%d]", curde->d_entry, curde->d_refcount);
		kprintf("',flags=0x%x, refcount=%d\n",
		 d->d_flags, d->d_refcount);
		n++;
	}
	kprintf("dentry cache contains %u entries\n", n);
}
#endif

INIT_FUNCTION(dcache_init, SUBSYSTEM_VFS, ORDER_FIRST);

/* vim:set ts=2 sw=2: */
