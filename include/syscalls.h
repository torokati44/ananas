/* This file is automatically generated by gen_syscalls.sh - do not edit! */
#define SYSCALL_exit 0
void sys_exit();
#define SYSCALL_read 1
ssize_t sys_read(int fd, void* buf, size_t len);
#define SYSCALL_write 2
ssize_t sys_write(int fd, const void* buf, size_t len);
#define SYSCALL_map 3
void* sys_map(size_t len);
#define SYSCALL_unmap 4
int sys_unmap(void* addr, size_t length);
