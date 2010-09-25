#include <ananas/stat.h>
#include <fcntl.h>
#include <ananas/handle.h>
#include <ananas/stat.h>
#include <syscalls.h>

int unlink(const char* path)
{
	void* handle = sys_open(path, 0 /* XXX */);
	if (handle == NULL)
		return -1;
	sys_remove(handle);
	return 0;
}