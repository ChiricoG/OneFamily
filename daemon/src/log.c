/*
 * OneFamily Daemon for PS4 (GoldHEN)
 * Logging Subsystem using Direct Syscalls and Safe Kernel Resolution
 */

#include "log.h"
#include "config.h"
#include "daemon_syscalls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>

#define DAEMON_LOG_PATH_PRIMARY  "/data/onefamily/onefamily.log"
#define DAEMON_LOG_PATH_FALLBACK "/user/data/onefamily/onefamily.log"
#define DAEMON_LOG_PREV_PRIMARY  "/data/onefamily/onefamily.prev.log"
#define DAEMON_LOG_PREV_FALLBACK "/user/data/onefamily/onefamily.prev.log"
#define DAEMON_LOG_MAX_BYTES     (1024U * 1024U)

static int g_log_fd = -1;
static int g_log_use_fallback = 0;
static size_t g_log_bytes = 0;

static void log_rotate_and_open(void) {
    if (!g_log_use_fallback) {
        daemon_sys_unlink(DAEMON_LOG_PREV_PRIMARY);
        daemon_sys_rename(DAEMON_LOG_PATH_PRIMARY, DAEMON_LOG_PREV_PRIMARY);
        g_log_fd = daemon_sys_open(DAEMON_LOG_PATH_PRIMARY, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (g_log_fd < 0) {
            g_log_use_fallback = 1;
        }
    }
    if (g_log_fd < 0 && g_log_use_fallback) {
        daemon_sys_unlink(DAEMON_LOG_PREV_FALLBACK);
        daemon_sys_rename(DAEMON_LOG_PATH_FALLBACK, DAEMON_LOG_PREV_FALLBACK);
        g_log_fd = daemon_sys_open(DAEMON_LOG_PATH_FALLBACK, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    g_log_bytes = 0;
}

void daemon_log_init(void) {
    if (g_log_fd < 0) {
        daemon_sys_mkdir("/data", 0777);
        daemon_sys_mkdir("/data/onefamily", 0777);
        daemon_sys_mkdir("/user", 0777);
        daemon_sys_mkdir("/user/data", 0777);
        daemon_sys_mkdir("/user/data/onefamily", 0777);

        log_rotate_and_open();
    }
}

void daemon_log_close(void) {
    if (g_log_fd >= 0) {
        daemon_sys_close(g_log_fd);
        g_log_fd = -1;
    }
}

static const char* level_to_str(int level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "LOG";
    }
}

void daemon_log(int level, const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    char full_msg[600];
    snprintf(full_msg, sizeof(full_msg), "[%s] %s\n", level_to_str(level), buffer);
    size_t len = strlen(full_msg);

    if (g_log_fd < 0) {
        daemon_log_init();
    } else if ((g_log_bytes + len) >= DAEMON_LOG_MAX_BYTES) {
        daemon_sys_close(g_log_fd);
        g_log_fd = -1;
        log_rotate_and_open();
    }

    if (g_log_fd >= 0) {
        int wr = daemon_sys_write(g_log_fd, full_msg, len);
        if (wr > 0) {
            g_log_bytes += (size_t)wr;
        }
    }
}

void daemon_notify(const char *fmt, ...) {
    if (!g_daemon_config.enable_notifications) {
        return;
    }

    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    daemon_log(LOG_LEVEL_INFO, "%s", buffer);
    daemon_kernel_notify(buffer);
}
