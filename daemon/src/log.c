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

#define DAEMON_LOG_PATH "/data/GoldHEN/onefamily.log"

static int g_log_fd = -1;

void daemon_log_init(void) {
    if (g_log_fd < 0) {
        g_log_fd = daemon_sys_open(DAEMON_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
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

    if (g_log_fd < 0) {
        daemon_log_init();
    }

    if (g_log_fd >= 0) {
        daemon_sys_write(g_log_fd, full_msg, strlen(full_msg));
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
