#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>

#include "common/graphics.h"
#include "common/log.h"
#include "config_manager.h"
#include "installer.h"
#include "ui.h"
#include "hid_profile.h"
#include "pkg_updater.h"

static void send_vibe_to_daemon(uint8_t lgMotor, uint8_t smMotor) {
    if (lgMotor == 0 && smMotor == 0) {
        sceKernelUnlink("/data/onefamily/onefamily.vibe");
        sceKernelUnlink("/user/data/onefamily/onefamily.vibe");
        sceKernelUnlink("/data/GoldHEN/onefamily.vibe");
    } else {
        mkdir("/data", 0777);
        mkdir("/data/onefamily", 0777);
        int fd = sceKernelOpen("/data/onefamily/onefamily.vibe", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            fd = sceKernelOpen("/user/data/onefamily/onefamily.vibe", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        }
        if (fd >= 0) {
            uint8_t data[2] = { lgMotor, smMotor };
            sceKernelWrite(fd, data, sizeof(data));
            sceKernelClose(fd);
        }
    }
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Initialize 2D Graphics (1920x1080 @ 60 Hz)
    Scene2D* scene = new Scene2D(1920, 1080, 4);
    if (!scene->Init(0xC000000, 2)) {
        return -1;
    }

    // Initialize Config Manager
    // NOTE: If /data/onefamily/onefamily.ini does not exist,
    // configMgr.load() automatically creates it with all default settings!
    ConfigManager configMgr;
    configMgr.load();

    // Initialize UI
    UI* ui = new UI();
    if (!ui->init(scene)) {
        return -1;
    }

    // Initialize Controller via OpenOrbis scePad
    scePadInit();
    int userId = -1;
    OrbisUserServiceInitializeParams uParam;
    uParam.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&uParam);
    sceUserServiceGetInitialUser(&userId);

    // Standard handle for navigating the app and reading controller telemetry
    int padHandle = scePadOpen(userId, 0, 0, NULL);

    // Check if First-Launch Setup Wizard or Payload Update is required
    bool isInstalled = Installer::isInstalled();
    bool isUpToDate = Installer::isPayloadUpToDate();

    if (!isInstalled || !isUpToDate) {
        bool isUpdate = isInstalled && !isUpToDate;
        InstallResult res = Installer::runInstallation(isUpdate);

        int wizardFrames = 0;
        uint32_t wizardPrevButtons = 0;

        for (;;) {
            OrbisPadData padData;
            memset(&padData, 0, sizeof(padData));
            if (padHandle >= 0) {
                scePadReadState(padHandle, &padData);
            }

            uint32_t curButtons = padData.buttons;
            uint32_t justPressed = curButtons & ~wizardPrevButtons;
            wizardPrevButtons = curButtons;

            // Exit on CROSS, CIRCLE, or OPTIONS
            if (justPressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE | ORBIS_PAD_BUTTON_OPTIONS)) {
                break;
            }

            ui->render_wizard(scene, res, wizardFrames);

            scene->SubmitFlip(wizardFrames);
            scene->FrameWait(wizardFrames);
            scene->FrameBufferSwap();
            wizardFrames++;
        }

        if (padHandle >= 0) {
            scePadClose(padHandle);
        }
        sceSystemServiceLoadExec("exit", NULL);
        exit(0);
        return 0;
    }

    int activeTab = 0;
    int selectedCard = 0;
    int toastFrames = 0;
    std::string toastText = "Settings saved to /data/onefamily/onefamily.ini";
    int frameID = 0;
    uint32_t prevButtons = 0;
    TesterState tester;

    HidStatus hidStatus;
    HidProfile::read_status(hidStatus);
    HidProfile hidProfile;
    bool hidLoaded = false;
    int remapSelectedRow = 0;
    bool remapTestMode = false;

    // Start background check for online PKG updates
    PkgUpdater& updater = PkgUpdater::instance();
    updater.start_check();

    // Main Interaction Loop
    for (;;) {
        OrbisPadData padData;
        memset(&padData, 0, sizeof(padData));
        if (padHandle >= 0) {
            scePadReadState(padHandle, &padData);
        }

        uint32_t curButtons = padData.buttons;
        uint32_t justPressed = curButtons & ~prevButtons;
        prevButtons = curButtons;

        // Poll HID controller status every 20 frames (~3 Hz)
        if (frameID % 20 == 0) {
            HidProfile::read_status(hidStatus);
        }
        bool isHidType = (hidStatus.type == "generic_hid" ||
                          hidStatus.type == "Generic HID Gamepad" ||
                          hidStatus.type.find("hid") != std::string::npos ||
                          hidStatus.type.find("HID") != std::string::npos);
        bool hasHid = (hidStatus.connected && isHidType);
        if (hasHid && !hidLoaded) {
            hidProfile.load(hidStatus.vid, hidStatus.pid, hidStatus.nbuttons);
            hidLoaded = true;
        } else if (!hasHid) {
            hidLoaded = false;
        }

        int numTabs = hasHid ? 4 : 3;
        if (activeTab >= numTabs) {
            activeTab = numTabs - 1;
        }

        AppConfig& cfg = configMgr.getConfig();

        // Exit on OPTIONS or CIRCLE (suspended during Remap Test Mode)
        if (!remapTestMode && ((justPressed & ORBIS_PAD_BUTTON_OPTIONS) || (justPressed & ORBIS_PAD_BUTTON_CIRCLE))) {
            break;
        }

        // Tab Switching via L1 / R1 (suspended during Remap Test Mode)
        if (!remapTestMode) {
            if (justPressed & ORBIS_PAD_BUTTON_L1) {
                activeTab = (activeTab + numTabs - 1) % numTabs;
                if (activeTab != 1 && tester.rumblePhase != 0) {
                    tester.rumbleFrames = 0;
                    tester.rumblePhase = 0;
                    OrbisPadVibeParam vibe = { 0, 0 };
                    if (padHandle >= 0) scePadSetVibration(padHandle, &vibe);
                    send_vibe_to_daemon(0, 0);
                }
            }
            if (justPressed & ORBIS_PAD_BUTTON_R1) {
                activeTab = (activeTab + 1) % numTabs;
                if (activeTab != 1 && tester.rumblePhase != 0) {
                    tester.rumbleFrames = 0;
                    tester.rumblePhase = 0;
                    OrbisPadVibeParam vibe = { 0, 0 };
                    if (padHandle >= 0) scePadSetVibration(padHandle, &vibe);
                    send_vibe_to_daemon(0, 0);
                }
            }
        }

        if (activeTab != 2) {
            remapTestMode = false;
        }

        if (activeTab == 0) {
            const int TOTAL_CARDS = 5;

            // Navigate Cards in Settings
            if (justPressed & ORBIS_PAD_BUTTON_UP) {
                selectedCard = (selectedCard + TOTAL_CARDS - 1) % TOTAL_CARDS;
            }
            if (justPressed & ORBIS_PAD_BUTTON_DOWN) {
                selectedCard = (selectedCard + 1) % TOTAL_CARDS;
            }

            // Value changes (LEFT)
            if (justPressed & ORBIS_PAD_BUTTON_LEFT) {
                if (selectedCard == 0) {
                    if (cfg.deadzone_index > 0) cfg.deadzone_index--;
                } else if (selectedCard == 1) {
                    if (cfg.trigger_index > 0) cfg.trigger_index--;
                } else if (selectedCard == 2) {
                    cfg.swap_index ^= 1;
                } else if (selectedCard == 3) {
                    cfg.rumble_index ^= 1;
                } else if (selectedCard == 4) {
                    cfg.switch_index ^= 1;
                }
            }

            // Value changes (RIGHT)
            if (justPressed & ORBIS_PAD_BUTTON_RIGHT) {
                if (selectedCard == 0) {
                    if (cfg.deadzone_index < (int)ConfigManager::DEADZONE_OPTIONS.size() - 1) {
                        cfg.deadzone_index++;
                    }
                } else if (selectedCard == 1) {
                    if (cfg.trigger_index < (int)ConfigManager::TRIGGER_OPTIONS.size() - 1) {
                        cfg.trigger_index++;
                    }
                } else if (selectedCard == 2) {
                    cfg.swap_index ^= 1;
                } else if (selectedCard == 3) {
                    cfg.rumble_index ^= 1;
                } else if (selectedCard == 4) {
                    cfg.switch_index ^= 1;
                }
            }

            // Save on CROSS (X)
            if (justPressed & ORBIS_PAD_BUTTON_CROSS) {
                if (configMgr.save()) {
                    toastText = "Settings saved to /data/onefamily/onefamily.ini";
                    toastFrames = 180; // ~3 seconds toast feedback
                }
            }
        } else if (activeTab == 1) {
            // --- TESTER & CALIBRATION TAB ---
            // Square: Start Vibration Test
            if (justPressed & ORBIS_PAD_BUTTON_SQUARE) {
                tester.rumbleFrames = 180; // 3 seconds
            }

            // Triangle: Start Drift Auto-Calibration
            if (justPressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                tester.calib.active = true;
                tester.calib.completed = false;
                tester.calib.currentFrame = 0;
                tester.calib.maxNoise = 0;
            }

            // Cross: Apply suggested deadzone if calibration completed
            if (justPressed & ORBIS_PAD_BUTTON_CROSS) {
                if (tester.calib.completed) {
                    cfg.deadzone_index = tester.calib.suggestedIndex;
                    if (configMgr.save()) {
                        toastText = "Deadzone auto-calibrated & saved!";
                        toastFrames = 180;
                    }
                    tester.calib.completed = false;
                }
            }

            // Real-time processing for Rumble Test
            if (tester.rumbleFrames > 0) {
                tester.rumbleFrames--;
                OrbisPadVibeParam vibe;
                if (tester.rumbleFrames > 120) {
                    // Phase 1: Left heavy motor
                    tester.rumblePhase = 1;
                    vibe.lgMotor = 255;
                    vibe.smMotor = 0;
                } else if (tester.rumbleFrames > 60) {
                    // Phase 2: Right light motor
                    tester.rumblePhase = 2;
                    vibe.lgMotor = 0;
                    vibe.smMotor = 255;
                } else {
                    // Phase 3: Both motors
                    tester.rumblePhase = 3;
                    vibe.lgMotor = 255;
                    vibe.smMotor = 255;
                }
                if (padHandle >= 0) scePadSetVibration(padHandle, &vibe);
                send_vibe_to_daemon(vibe.lgMotor, vibe.smMotor);
            } else if (tester.rumblePhase != 0) {
                tester.rumblePhase = 0;
                OrbisPadVibeParam vibe = { 0, 0 };
                if (padHandle >= 0) scePadSetVibration(padHandle, &vibe);
                send_vibe_to_daemon(0, 0);
            }

            // Real-time processing for Drift Calibration
            if (tester.calib.active) {
                int devLX = abs((int)padData.leftStick.x - 128);
                int devLY = abs((int)padData.leftStick.y - 128);
                int devRX = abs((int)padData.rightStick.x - 128);
                int devRY = abs((int)padData.rightStick.y - 128);

                if (devLX > tester.calib.maxNoise) tester.calib.maxNoise = devLX;
                if (devLY > tester.calib.maxNoise) tester.calib.maxNoise = devLY;
                if (devRX > tester.calib.maxNoise) tester.calib.maxNoise = devRX;
                if (devRY > tester.calib.maxNoise) tester.calib.maxNoise = devRY;

                tester.calib.currentFrame++;
                if (tester.calib.currentFrame >= tester.calib.totalFrames) {
                    tester.calib.active = false;
                    tester.calib.completed = true;

                    // Find smallest deadzone >= (maxNoise + 2) in DEADZONE_VALUES
                    int needed = tester.calib.maxNoise + 2;
                    int bestIdx = 0;
                    for (size_t i = 0; i < ConfigManager::DEADZONE_VALUES.size(); i++) {
                        if (ConfigManager::DEADZONE_VALUES[i] >= needed) {
                            bestIdx = (int)i;
                            break;
                        }
                        bestIdx = (int)i;
                    }
                    tester.calib.suggestedIndex = bestIdx;
                }
            }
        } else if (activeTab == 2 && hasHid) {
            // --- REMAP HID TAB ---
            // R3 toggles between Edit Mode and Test Mode (no intrusive toast needed)
            if (justPressed & ORBIS_PAD_BUTTON_R3) {
                remapTestMode = !remapTestMode;
            }

            if (!remapTestMode) {
                if (justPressed & ORBIS_PAD_BUTTON_UP) {
                    remapSelectedRow = (remapSelectedRow + ACT_COUNT - 1) % ACT_COUNT;
                }
                if (justPressed & ORBIS_PAD_BUTTON_DOWN) {
                    remapSelectedRow = (remapSelectedRow + 1) % ACT_COUNT;
                }
                if (justPressed & ORBIS_PAD_BUTTON_LEFT) {
                    hidProfile.cycle_button(remapSelectedRow, -1);
                }
                if (justPressed & ORBIS_PAD_BUTTON_RIGHT) {
                    hidProfile.cycle_button(remapSelectedRow, 1);
                }
                if (justPressed & ORBIS_PAD_BUTTON_CROSS) {
                    if (hidProfile.save(hidStatus.vid, hidStatus.pid, hidStatus.name)) {
                        toastText = "Profile saved & active!";
                        toastFrames = 180;
                    }
                }
                if (justPressed & ORBIS_PAD_BUTTON_SQUARE) {
                    hidProfile.reset_defaults(hidStatus.nbuttons);
                    toastText = "Defaults restored (press X to save)";
                    toastFrames = 180;
                }
                if (justPressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                    hidProfile.load(hidStatus.vid, hidStatus.pid, hidStatus.nbuttons);
                    toastText = "Profile reloaded from disk";
                    toastFrames = 180;
                }
            }
        } else {
            // In Info tab: TRIANGLE triggers PKG download if update is available
            if ((justPressed & ORBIS_PAD_BUTTON_TRIANGLE) && updater.get_state() == UPDATER_UPDATE_AVAILABLE) {
                updater.start_download();
                toastText = "Download started: onefamily_companion.pkg -> /data/pkg/";
                toastFrames = 180;
            }

            // In Info tab: SQUARE re-runs installation & ensures files/autoboot are up-to-date
            if (justPressed & ORBIS_PAD_BUTTON_SQUARE) {
                Installer::runInstallation();
                toastText = "OneFamily reinstalled to /data/payloads/onefamily.bin";
                toastFrames = 180;
            }

            // In Info tab: CROSS or LEFT returns to Settings
            if ((justPressed & ORBIS_PAD_BUTTON_CROSS) || (justPressed & ORBIS_PAD_BUTTON_LEFT)) {
                activeTab = 0;
            }
        }

        if (toastFrames > 0) {
            toastFrames--;
        }

        // Render UI
        ui->render(scene, cfg, activeTab, selectedCard, toastFrames, padData, tester, toastText.c_str(),
                   hasHid, &hidStatus, &hidProfile, remapSelectedRow, remapTestMode, &updater);

        // Flip & VSync
        scene->SubmitFlip(frameID);
        scene->FrameWait(frameID);
        scene->FrameBufferSwap();
        frameID++;
    }

    if (padHandle >= 0) {
        OrbisPadVibeParam vibe = { 0, 0 };
        scePadSetVibration(padHandle, &vibe);
        send_vibe_to_daemon(0, 0);
        scePadClose(padHandle);
    } else {
        send_vibe_to_daemon(0, 0);
    }

    // Gracefully exit back to PS4 shell without CE-34878-0
    sceSystemServiceLoadExec("exit", NULL);
    exit(0);

    return 0;
}
