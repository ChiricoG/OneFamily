#ifndef DAEMON_CONFIG_H
#define DAEMON_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "ini_parser.h"

typedef PluginConfig DaemonConfig;

#define DEFAULT_STICK_DEADZONE    15
#define DEFAULT_TRIGGER_THRESHOLD 30

extern DaemonConfig g_daemon_config;

void daemon_config_init_defaults(DaemonConfig *cfg);
void daemon_config_load(DaemonConfig *cfg);
bool daemon_config_check_reload(DaemonConfig *cfg);

#endif // DAEMON_CONFIG_H
