#ifndef DAEMON_SYSCALLS_H
#define DAEMON_SYSCALLS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

long daemon_syscall(long num, ...);

int daemon_sys_open(const char *path, int flags, int mode);
int daemon_sys_read(int fd, void *buf, size_t count);
int daemon_sys_write(int fd, const void *buf, size_t count);
int daemon_sys_close(int fd);
int daemon_sys_nanosleep(uint64_t ns);
int daemon_sys_usleep(uint32_t us);
uint64_t daemon_sys_time_us(void);
int daemon_sys_load_prx(const char *name, int *out_handle);
int daemon_sys_dlsym(int handle, const char *symbol, void **out_addr);

// Resolves libkernel functions dynamically (safe from dynamic link issues)
void daemon_kernel_init(void);
int daemon_kernel_notify(const char *message);
int daemon_kernel_thread_create(void *out_thread, void *(*func)(void*), void *arg, const char *name);

#ifdef __cplusplus
}
#endif

#endif // DAEMON_SYSCALLS_H
