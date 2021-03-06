#ifndef __ANANAS_VFS_TYPES_H__
#define __ANANAS_VFS_TYPES_H__

#include <ananas/list.h>
#include <ananas/lock.h>
#include <ananas/stat.h> /* for 'struct stat' */
#include <ananas/vfs/dentry.h> /* for 'struct DENTRY_QUEUE' */
#include <ananas/vfs/icache.h> /* for 'struct ICACHE_QUEUE' */
#include <ananas/vfs/dirent.h>
#include <ananas/vmpage.h>

namespace Ananas {
class Device;
};
struct DENTRY;
struct VFS_MOUNTED_FS;
struct VFS_INODE_OPS;
struct VFS_FILESYSTEM_OPS;

#define INODE_LOCK(i) \
	mutex_lock(&(i)->i_mutex)
#define INODE_UNLOCK(i) \
	mutex_unlock(&(i)->i_mutex)

/*
 * VFS_INODE refers to an inode; it must be locked before any fields can
 * be updated. The refcount protects the inode from disappearing while
 * it is still being used.
 */
struct VFS_INODE {
	LIST_FIELDS(struct VFS_INODE);		/* Cache entries */
	mutex_t		i_mutex;		/* Mutex protecting inode */
	refcount_t	i_refcount;		/* Refcount, must be >=0 */
	unsigned int	i_flags;		/* Inode flags */
#define INODE_FLAG_DIRTY	(1 << 0)	/* Needs to be written */
#define INODE_FLAG_PENDING	(1 << 1)	/* Needs to be filled */
#define INODE_FLAG_GONE (1 << 2) /* No longer valid */
	struct stat 	i_sb;			/* Inode information */
	struct VFS_INODE_OPS* i_iops;		/* Inode operations */

	struct VFS_MOUNTED_FS* i_fs;		/* Filesystem where the inode lives */
	void*		i_privdata;		/* Filesystem-specific data */
	ino_t		i_inum;			/* Inode number */

	struct VM_PAGE_LIST	i_pages;	/* Backing VM pages, if any */
};

/*
 * VFS_FILE refers to an opened file. It will have an associated
 * inode and a position.
 */
struct VFS_FILE {
	off_t			f_offset;
	/*
	 * An opened file can have an inode or device as backend; we'll use the
	 * dentry instead of the inode because we need its name when
	 * unlink()-ing it; plus the dentry also contains the parent (which is
	 * useful when resolving the item back to a path)
	 */
	struct DENTRY*		f_dentry;
	Ananas::Device*		f_device;
};

/*
 * VFS_MOUNTED_FS is used to refer to a mounted filesystem. Note that
 * we will also use it during the filesystem-operations mount() call.
 *
 * Fields marked with (R) are readonly and must never be changed after
 * mounting.
 */
struct VFS_MOUNTED_FS {
	spinlock_t	fs_spinlock;		/* Protects fields marked with (F) */
	Ananas::Device*	fs_device;		/* (F) Device where the filesystem lives */
	unsigned int	fs_flags;		/* (F) Filesystem flags */
#define VFS_FLAG_INUSE    0x0001		/* Filesystem entry is in use */
#define VFS_FLAG_READONLY 0x0002		/* Filesystem is readonly */
#define VFS_FLAG_ABANDONED	0x0004		/* Filesystem is no longer available */
	const char*	fs_mountpoint;		/* (R) Mount point */
	uint32_t	fs_block_size;		/* (R) Block size */
	void*		fs_privdata;		/* (R) Private filesystem data */

	struct VFS_FILESYSTEM_OPS* fs_fsops;		/* (R) Filesystem operations */
	struct DENTRY* fs_root_dentry;			/* (R) Filesystem's root dentry */
};

/*
 * VFS_FILESYSTEM_OPS are operations corresponding to the highest level:
 * mount/unmount a filesystem and obtaining statistics.
 */
struct VFS_FILESYSTEM_OPS {
	/*
	 * Mount a filesystem. device, mountpoint and fsops in 'fs' are
	 * guaranteed to be filled out. Must fill out the root inode on success.
	 */
	errorcode_t (*mount)(struct VFS_MOUNTED_FS* fs, struct VFS_INODE** root_inode);

	/*
	 * Initialize an inode. The purpose for this function is to initialize
	 * the 'privdata' field of the inode. Only i_num is available at this point.
	 */
	errorcode_t (*prepare_inode)(struct VFS_INODE* inode);

	/*
	 * Destroy a locked inode. The purpose for this function is to deinitialize
	 * the 'privdata' field of the inode. The function should end by
	 * calling vfs_discard_inode() to remove the inode itself.
	 */
	void (*discard_inode)(struct VFS_INODE* inode);

	/*
	 * Read an inode from disk; inode is locked and pre-allocated using
	 * alloc_inode().  The 'fs' field of the inode is guaranteed to be
	 * filled out.
	 */
	errorcode_t (*read_inode)(struct VFS_INODE* inode, ino_t num);

	/*
	 * Writes an inode back to disk; inode is locked.
	 */
	errorcode_t (*write_inode)(struct VFS_INODE* inode);
};

struct VFS_INODE_OPS {
	/*
	 * Reads directory entries. Must set length to amount of data filled on
	 * success.
	 */
	errorcode_t (*readdir)(struct VFS_FILE* file, void* buf, size_t* length);

	/*
	 * Looks up an entry within a directory, updates 'destinode' on success.
	 */
	errorcode_t (*lookup)(struct DENTRY* parent, struct VFS_INODE** destinode, const char* dentry);

	/*
	 * Maps the inode's given block number to a block device's block
	 * number. A new block is to be allocated if create is non-zero.
	 */
	errorcode_t (*block_map)(struct VFS_INODE* inode, blocknr_t block_in, blocknr_t* block_out, int create);

	/*
	 * Reads inode data to a buffer, up to len bytes. Must update len on success
	 * with the amount of data read.
	 */
	errorcode_t (*read)(struct VFS_FILE* file, void* buf, size_t* len);

	/*
	 * Writes inode data from a buffer, up to len bytes. Must update len on success
	 * wit hthe amount of data written.
	 */
	errorcode_t (*write)(struct VFS_FILE* file, const void* buf, size_t* len);

	/*
	 * Creates a new entry in the directory. On success, calls
	 * dentry_set_inode() to fill out the entry's inode.
	 */
	errorcode_t (*create)(struct VFS_INODE* dir, struct DENTRY* de, int mode);

	/*
	 * Removes an entry from a directory.
	 */
	errorcode_t (*unlink)(struct VFS_INODE* dir, struct DENTRY* de);

	/*
	 * Renames an entry.
	 */
	errorcode_t (*rename)(struct VFS_INODE* old_dir, struct DENTRY* old_dentry, struct VFS_INODE* new_dir, struct DENTRY* new_dentry);

	/*
	 * Fills out the file structure.
	 */
	void (*fill_file)(struct VFS_INODE* inode, struct VFS_FILE* file);
};

/* 
 * A VFS filesystem defines the name of a filesystem and the operations to use.
 */
struct VFS_FILESYSTEM {
	/* Filesystem name */
	const char* fs_name;
	/* Filesystem operations */
	struct VFS_FILESYSTEM_OPS* fs_fsops;
	LIST_FIELDS(struct VFS_FILESYSTEM);
};

LIST_DEFINE(VFS_FILESYSTEMS, struct VFS_FILESYSTEM);

#endif /* __ANANAS_VFS_TYPES_H__ */
