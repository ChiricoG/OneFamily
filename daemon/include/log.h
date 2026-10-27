#ifndef DAEMON_LOG_H
#define DAEMON_LOG_H

#include <stdint.h>

#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

void daemon_log_init(void);
void daemon_log_close(void);
void daemon_log(int level, const char *fmt, ...);
void daemon_notify(const char *fmt, ...);

#endif // DAEMON_LOG_H
