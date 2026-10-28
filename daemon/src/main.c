/*
 * OneFamily Daemon for PS4 (GoldHEN)
 * Entry Point and Service Manager
 */

#include "vpad.h"
#include "usb_driver.h"
#include "config.h"
#include "log.h"
#include "daemon_syscalls.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static volatile bool g_running = true;

static void check_vibe_file(void) {
    static uint8_t s_last_lg = 0;
    static uint8_t s_last_sm = 0;
    static int s_had_file = 0;

    int fd = daemon_sys_open("/data/GoldHEN/onefamily.vibe", 0 /* O_RDONLY */, 0);
    if (fd >= 0) {
        uint8_t vibe[2] = {0, 0};
        int n = daemon_sys_read(fd, vibe, sizeof(vibe));
        daemon_sys_close(fd);
        if (n >= 2) {
            s_had_file = 1;
            if (vibe[0] != s_last_lg || vibe[1] != s_last_sm) {
                s_last_lg = vibe[0];
                s_last_sm = vibe[1];
                usb_driver_send_rumble(s_last_lg, s_last_sm);
            }
        }
    } else {
        if (s_had_file) {
            s_had_file = 0;
            s_last_lg = 0;
            s_last_sm = 0;
            usb_driver_send_rumble(0, 0);
        }
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // 1. Initialize Safe Kernel resolution and Logging
    daemon_log_init();
    daemon_log(LOG_LEVEL_INFO, "=== OneFamily Daemon v1.1.5-beta2 (Native) Starting ===");
    daemon_kernel_init();

    // 2. Load user configuration from /data/GoldHEN/onefamily.ini (fallback: xbox_controller.ini)
    daemon_config_load(&g_daemon_config);
    daemon_log(LOG_LEVEL_INFO, "Configuration loaded: guide_action=%d, deadzone=%d, trigger=%d, rumble=%d",
               g_daemon_config.guide_action,
               g_daemon_config.stick_deadzone,
               g_daemon_config.trigger_threshold,
               g_daemon_config.enable_rumble);

    // 3. Notify user on screen immediately
    daemon_notify("OneFamily Daemon v1.1.5-beta2 Active");

    // 4. Initialize USB Driver Subsystem
    if (usb_driver_init() != 0) {
        daemon_log(LOG_LEVEL_ERROR, "Failed to initialize USB Driver");
    }

    daemon_log(LOG_LEVEL_INFO, "=== Daemon Ready - Entering USB Polling Loop (250 Hz) ===");

    uint32_t loop_count = 0;

    // Main service loop (250 Hz)
    while (g_running) {
        // Poll USB controller and feed Virtual DualShock 4
        usb_driver_poll_step();

        // Poll virtual DS4 rumble from OS/games
        vpad_poll_rumble();

        // Poll companion app vibration test IPC (~50 Hz = every 5 loops)
        if (++loop_count % 5 == 0) {
            check_vibe_file();
        }

        // Periodically check if configuration INI file was updated (every ~1s = 250 loops)
        if (loop_count % 250 == 0) {
            daemon_config_check_reload(&g_daemon_config);
        }

        daemon_sys_usleep(4000); // 4 ms sleep = 250 Hz
    }

    daemon_log(LOG_LEVEL_INFO, "Shutting down OneFamily Daemon...");
    usb_driver_shutdown();
    vpad_shutdown();
    daemon_log_close();

    return 0;
}
