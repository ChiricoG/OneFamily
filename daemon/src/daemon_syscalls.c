/*
 * Crash-Proof System Call and Dynamic Resolution Layer for OneFamily Daemon Payload
 * Avoids any dependency on unlinked PLT entries (__error, libkernel.so stubs).
 */

#include "daemon_syscalls.h"
#include "log.h"
#include <string.h>

__asm__(
    ".global _start\n"
    "_start:\n"
    "  pushq %rbp\n"
    "  movq %rsp, %rbp\n"
    "  andq $-16, %rsp\n"     /* 16-byte stack alignment */
    "  xorl %edi, %edi\n"     /* argc = 0 */
    "  xorl %esi, %esi\n"     /* argv = NULL */
    "  callq main\n"
    "  movq %rbp, %rsp\n"
    "  popq %rbp\n"
    "  retq\n"
    "\n"
    ".global daemon_syscall\n"
    "daemon_syscall:\n"
    "  movq %rdi, %rax\n"     /* rax = syscall number */
    "  movq %rsi, %rdi\n"     /* arg1 */
    "  movq %rdx, %rsi\n"     /* arg2 */
    "  movq %rcx, %rdx\n"     /* arg3 */
    "  movq %r8, %r10\n"      /* arg4 (rcx -> r10 for kernel syscall) */
    "  movq %r9, %r8\n"       /* arg5 */
    "  movq 8(%rsp), %r9\n"   /* arg6 */
    "  syscall\n"
    "  jnc 1f\n"
    "  negq %rax\n"           /* return negative error on failure */
    "1:\n"
    "  retq\n"
);

int daemon_sys_open(const char *path, int flags, int mode) {
    return (int)daemon_syscall(5, path, flags, mode);
}

int daemon_sys_read(int fd, void *buf, size_t count) {
    return (int)daemon_syscall(3, fd, buf, count);
}

int daemon_sys_write(int fd, const void *buf, size_t count) {
    return (int)daemon_syscall(4, fd, buf, count);
}

int daemon_sys_close(int fd) {
    return (int)daemon_syscall(6, fd);
}

int daemon_sys_mkdir(const char *path, int mode) {
    return (int)daemon_syscall(136, path, mode);
}

int daemon_sys_nanosleep(uint64_t ns) {
    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } req;
    req.tv_sec = (int64_t)(ns / 1000000000ULL);
    req.tv_nsec = (int64_t)(ns % 1000000000ULL);
    return (int)daemon_syscall(240, &req, 0);
}

int daemon_sys_usleep(uint32_t us) {
    return daemon_sys_nanosleep((uint64_t)us * 1000ULL);
}

uint64_t daemon_sys_time_us(void) {
    struct {
        int64_t sec;
        long nsec;
    } ts;
    daemon_syscall(232, 4 /* CLOCK_MONOTONIC */, &ts);
    return (uint64_t)ts.sec * 1000000ULL + (uint64_t)ts.nsec / 1000ULL;
}

uint64_t sceKernelGetProcessTime(void) {
    return daemon_sys_time_us();
}

static int s_dummy_errno = 0;
int *__error(void) {
    return &s_dummy_errno;
}

void *__tls_get_addr(void *m) {
    (void)m;
    return NULL;
}

int daemon_sys_load_prx(const char *name, int *out_handle) {
    int h = 0;
    long ret = daemon_syscall(594, name, 0, &h, 0);
    if (ret >= 0 && h != 0) {
        if (out_handle) *out_handle = h;
        return 0;
    }
    return (int)ret;
}

int daemon_sys_dlsym(int handle, const char *symbol, void **out_addr) {
    void *addr = 0;
    long ret = daemon_syscall(591, handle, symbol, &addr);
    if (ret >= 0 && addr != 0) {
        if (out_addr) *out_addr = addr;
        return 0;
    }
    return (int)ret;
}

// -------------------------------------------------------------
// Runtime Resolution from libkernel.sprx
// -------------------------------------------------------------

typedef struct {
    int32_t type;
    int32_t reqId;
    int32_t priority;
    int32_t msgId;
    int32_t targetId;
    int32_t userId;
    int32_t unk1;
    int32_t unk2;
    int32_t appId;
    int32_t errorNum;
    int32_t unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
} KernelNotificationRequest;

typedef int (*sceKernelSendNotificationRequest_t)(int, KernelNotificationRequest*, size_t, int);
typedef int (*scePthreadCreate_t)(void*, const void*, void* (*)(void*), void*, const char*);

static int s_hKernel = 0;
static sceKernelSendNotificationRequest_t p_send_notif = NULL;
static scePthreadCreate_t p_pthread_create = NULL;

void daemon_kernel_init(void) {
    if (s_hKernel != 0 && p_send_notif != NULL && p_pthread_create != NULL) return;

    // 1. Try loading libkernel
    int h = 0;
    daemon_sys_load_prx("/system/common/lib/libkernel.sprx", &h);
    if (h == 0) daemon_sys_load_prx("libkernel.sprx", &h);
    if (h == 0) daemon_sys_load_prx("/system/common/lib/libkernel_sys.sprx", &h);
    if (h == 0) daemon_sys_load_prx("libkernel_sys.sprx", &h);

    if (h != 0) {
        s_hKernel = h;
        daemon_sys_dlsym(s_hKernel, "sceKernelSendNotificationRequest", (void**)&p_send_notif);
        daemon_sys_dlsym(s_hKernel, "scePthreadCreate", (void**)&p_pthread_create);
    }

    // 2. Fallback: query loaded module list via syscall 592
    if (!p_send_notif || !p_pthread_create) {
        int handles[64];
        int count = 0;
        if (daemon_syscall(592, handles, 64, &count) == 0 && count > 0) {
            for (int i = 0; i < count; i++) {
                if (!p_send_notif) {
                    daemon_sys_dlsym(handles[i], "sceKernelSendNotificationRequest", (void**)&p_send_notif);
                }
                if (!p_pthread_create) {
                    daemon_sys_dlsym(handles[i], "scePthreadCreate", (void**)&p_pthread_create);
                }
                if (p_send_notif && p_pthread_create) {
                    s_hKernel = handles[i];
                    break;
                }
            }
        }
    }

    daemon_log(LOG_LEVEL_INFO, "Kernel Subsystem: s_hKernel=0x%X, notif=%p, pthread=%p",
               s_hKernel, p_send_notif, p_pthread_create);
}

int daemon_kernel_notify(const char *message) {
    daemon_kernel_init();
    if (!p_send_notif || !message) return -1;

    KernelNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = 0; // NotificationRequest
    req.targetId = -1;
    strncpy(req.message, message, sizeof(req.message) - 1);

    return p_send_notif(0, &req, sizeof(req), 0);
}

int daemon_kernel_thread_create(void *out_thread, void *(*func)(void*), void *arg, const char *name) {
    daemon_kernel_init();
    if (!p_pthread_create) return -1;
    return p_pthread_create(out_thread, NULL, func, arg, name);
}
