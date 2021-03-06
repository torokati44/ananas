#ifndef ANANAS_VFS_DIRENT_H
#define ANANAS_VFS_DIRENT_H

#include <ananas/types.h>

/*
 * Directory entry, as returned by the kernel.
 */
struct VFS_DIRENT {
	uint32_t	de_flags;		/* Flags */
	uint8_t		de_name_length;		/* Length of name */
	ino_t		de_inum;		/* Identifier */
	/*
	 * de_name will be stored directly after the fsop.
	 */
	char	de_name[1];
};
#define DE_LENGTH(x) (sizeof(struct VFS_DIRENT) + (x)->de_name_length)

#endif // ANANAS_VFS_DIRENT_H
