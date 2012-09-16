#include <ananas/types.h>
#include <ananas/error.h>
#include <ananas/syscalls.h>
#include <_posix/error.h>
#include <_posix/handlemap.h>
#include <errno.h>
#include <unistd.h>

ssize_t read(int fd, void* buf, size_t len)
{
	void* handle = handlemap_deref(fd, HANDLEMAP_TYPE_ANY);
	struct HANDLEMAP_OPS* hops = handlemap_get_ops(fd);
	if (handle == NULL || hops == NULL || hops->hop_read == NULL) {
		errno = EBADF; /* XXX is this correct? */
		return -1;
	}
	return hops->hop_read(fd, handle, buf, len);
}
