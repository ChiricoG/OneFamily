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

static void notify_daemon_reload() {
    int rfd1 = sceKernelOpen("/data/onefamily/onefamily.reload", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (rfd1 >= 0) {
        sceKernelWrite(rfd1, "1\n", 2);
        sceKernelClose(rfd1);
    }
    int rfd2 = sceKernelOpen("/user/data/onefamily/onefamily.reload", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (rfd2 >= 0) {
        sceKernelWrite(rfd2, "1\n", 2);
        sceKernelClose(rfd2);
    }
}

static void send_vibe_to_daemon(uint8_t lgMotor, uint8_t smMotor) {
    static uint8_t s_last_sent_lg = 0;
    static uint8_t s_last_sent_sm = 0;
    static bool s_has_sent = false;

    if (s_has_sent && lgMotor == s_last_sent_lg && smMotor == s_last_sent_sm) {
        return; // State unchanged, avoid spamming filesystem I/O on 60fps render loop
    }
    s_last_sent_lg = lgMotor;
    s_last_sent_sm = smMotor;
    s_has_sent = true;

    if (lgMotor == 0 && smMotor == 0) {
        sceKernelUnlink("/data/onefamily/onefamily.vibe");
        sceKernelUnlink("/user/data/onefamily/onefamily.vibe");
        sceKernelUnlink("/data/GoldHEN/onefamily.vibe");
        s_has_sent = false;
    } else {
        mkdir("/data", 0777);
        mkdir("/data/onefamily", 0777);
        mkdir("/user", 0777);
        mkdir("/user/data", 0777);
        mkdir("/user/data/onefamily", 0777);
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

static void refresh_pad_handles(int padHandles[4], int initialUserId) {
    OrbisUserServiceLoginUserIdList userList;
    memset(&userList, 0, sizeof(userList));
    bool hasUserList = (sceUserServiceGetLoginUserIdList(&userList) == 0);

    for (int i = 0; i < 4; i++) {
        // If handle is already valid and active, verify if it is alive
        if (padHandles[i] >= 0) {
            OrbisPadData testData;
            memset(&testData, 0, sizeof(testData));
            if (scePadReadState(padHandles[i], &testData) == 0 && testData.connected) {
                continue;
            }
            scePadClose(padHandles[i]);
            padHandles[i] = -1;
        }

        // Open controller for user slot i if that user is logged in
        if (hasUserList && userList.userId[i] != ORBIS_USER_SERVICE_USER_ID_INVALID) {
            padHandles[i] = scePadOpen(userList.userId[i], 0, 0, NULL);
        } else if (!hasUserList && i == 0 && initialUserId >= 0) {
            // Fallback strictly for Player 1 only if user listing failed
            padHandles[0] = scePadOpen(initialUserId, 0, 0, NULL);
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

    // Standard handles for up to 4 player controller slots
    int padHandles[4];
    for (int i = 0; i < 4; i++) padHandles[i] = -1;
    refresh_pad_handles(padHandles, userId);

    // Check if First-Launch Setup Wizard or Payload Update is required
    bool isInstalled = Installer::isInstalled();
    bool isUpToDate = Installer::isPayloadUpToDate();

    if (!isInstalled || !isUpToDate) {
        bool isUpdate = isInstalled && !isUpToDate;
        InstallResult res = Installer::runInstallation(isUpdate);

        int wizardFrames = 0;
        uint32_t wizardPrevButtons = 0;

        for (;;) {
            uint32_t anyWizardButtons = 0;
            for (int i = 0; i < 4; i++) {
                if (padHandles[i] >= 0) {
                    OrbisPadData samples[16];
                    memset(samples, 0, sizeof(samples));
                    int n = scePadRead(padHandles[i], samples, 16);
                    if (n > 0) {
                        for (int s = 0; s < n; s++) {
                            if (samples[s].connected) anyWizardButtons |= samples[s].buttons;
                        }
                    } else {
                        OrbisPadData d;
                        memset(&d, 0, sizeof(d));
                        if (scePadReadState(padHandles[i], &d) == 0 && d.connected) {
                            anyWizardButtons |= d.buttons;
                        }
                    }
                }
            }

            uint32_t curButtons = anyWizardButtons;
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

        for (int i = 0; i < 4; i++) {
            if (padHandles[i] >= 0) {
                scePadClose(padHandles[i]);
            }
        }
        sceSystemServiceLoadExec("exit", NULL);
        exit(0);
        return 0;
    }

    int activeTabIndex = 0;
    int selectedCard = 0;
    int dsSelectedCard = 0;
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
    UpdaterState lastUpdaterState = UPDATER_IDLE;

    // Main Interaction Loop
    for (;;) {
        // Periodically refresh controller handles to support dynamic reconnects and multi-user login
        if (frameID % 30 == 0) {
            refresh_pad_handles(padHandles, userId);
        }

        OrbisPadData padDataSlots[4];
        uint32_t anyButtons = 0;
        for (int i = 0; i < 4; i++) {
            memset(&padDataSlots[i], 0, sizeof(padDataSlots[i]));
            if (padHandles[i] >= 0) {
                OrbisPadData samples[16];
                memset(samples, 0, sizeof(samples));
                int n = scePadRead(padHandles[i], samples, 16);
                if (n > 0) {
                    for (int s = 0; s < n; s++) {
                        if (samples[s].connected) {
                            anyButtons |= samples[s].buttons;
                        }
                    }
                    padDataSlots[i] = samples[n - 1];
                    tester.slotConnected[i] = samples[n - 1].connected;
                } else if (scePadReadState(padHandles[i], &padDataSlots[i]) == 0 && padDataSlots[i].connected) {
                    anyButtons |= padDataSlots[i].buttons;
                    tester.slotConnected[i] = true;
                } else {
                    tester.slotConnected[i] = false;
                }
            } else {
                tester.slotConnected[i] = false;
            }
        }

        // Smart Active Slot Auto-Detection:
        // Automatically switch tester.selectedSlot to whichever controller slot is actively sending inputs
        // (suspended when in Input Tester Test Mode to prevent accidental slot jumping)
        if (!tester.testMode) {
            for (int i = 0; i < 4; i++) {
                if (tester.slotConnected[i]) {
                    const OrbisPadData& d = padDataSlots[i];
                    bool hasStick = (abs((int)d.leftStick.x - 128) > 20 ||
                                     abs((int)d.leftStick.y - 128) > 20 ||
                                     abs((int)d.rightStick.x - 128) > 20 ||
                                     abs((int)d.rightStick.y - 128) > 20);
                    bool hasTrig = (d.analogButtons.l2 > 30 || d.analogButtons.r2 > 30);
                    bool hasBtn = (d.buttons != 0);
                    if (hasStick || hasTrig || hasBtn) {
                        tester.selectedSlot = i;
                        break;
                    }
                }
            }
        }
        if (tester.selectedSlot < 0 || tester.selectedSlot >= 4 || !tester.slotConnected[tester.selectedSlot]) {
            for (int i = 0; i < 4; i++) {
                if (tester.slotConnected[i]) {
                    tester.selectedSlot = i;
                    break;
                }
            }
        }
        if (tester.selectedSlot < 0 || tester.selectedSlot >= 4) {
            tester.selectedSlot = 0;
        }
        OrbisPadData padData = padDataSlots[tester.selectedSlot];

        // Left Analog Stick Navigation Emulation (Stick-to-DPad with Auto-Repeat)
        static int s_stickRepeatFrames = 0;
        static uint32_t s_stickNavButtons = 0;

        uint32_t stickNav = 0;
        const int STICK_NAV_THRESHOLD = 64; // < 64 or > 192 from center 128
        if (padData.leftStick.y < 128 - STICK_NAV_THRESHOLD) stickNav |= ORBIS_PAD_BUTTON_UP;
        else if (padData.leftStick.y > 128 + STICK_NAV_THRESHOLD) stickNav |= ORBIS_PAD_BUTTON_DOWN;

        if (padData.leftStick.x < 128 - STICK_NAV_THRESHOLD) stickNav |= ORBIS_PAD_BUTTON_LEFT;
        else if (padData.leftStick.x > 128 + STICK_NAV_THRESHOLD) stickNav |= ORBIS_PAD_BUTTON_RIGHT;

        uint32_t stickJustPressed = 0;
        // Suppress stick navigation emulation during Remap Test Mode or Tester Test Mode
        if (remapTestMode || tester.testMode) {
            s_stickNavButtons = 0;
            s_stickRepeatFrames = 0;
        } else if (stickNav != 0) {
            if (stickNav != s_stickNavButtons) {
                stickJustPressed = stickNav;
                s_stickNavButtons = stickNav;
                s_stickRepeatFrames = 20; // ~330ms delay
            } else {
                if (s_stickRepeatFrames > 0) {
                    s_stickRepeatFrames--;
                } else {
                    stickJustPressed = stickNav;
                    s_stickRepeatFrames = 8; // ~130ms repeat rate
                }
            }
        } else {
            s_stickNavButtons = 0;
            s_stickRepeatFrames = 0;
        }

        uint32_t curButtons = anyButtons;
        uint32_t justPressed = (curButtons & ~prevButtons) | stickJustPressed;
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
        bool hasDualSense = (hidStatus.connected && (hidStatus.type == "dualsense_bt" || hidStatus.type == "dualsense" || (hidStatus.vid == 0x054C && (hidStatus.pid == 0x0CE6 || hidStatus.pid == 0x0DF2))));

        if (hasHid && !hidLoaded) {
            hidProfile.load(hidStatus.vid, hidStatus.pid, hidStatus.nbuttons);
            hidLoaded = true;
        } else if (!hasHid) {
            hidLoaded = false;
        }

        // Dynamically build active tab list
        std::vector<int> activeTabs;
        activeTabs.push_back(TAB_SETTINGS);
        activeTabs.push_back(TAB_TESTER);
        if (hasDualSense) {
            activeTabs.push_back(TAB_DUALSENSE);
        }
        if (hasHid) {
            activeTabs.push_back(TAB_REMAP_HID);
        }
        activeTabs.push_back(TAB_INFO);

        int numTabs = (int)activeTabs.size();
        if (activeTabIndex >= numTabs) {
            activeTabIndex = numTabs - 1;
        }
        int currentTabId = activeTabs[activeTabIndex];

        AppConfig& cfg = configMgr.getConfig();

        // Exit on OPTIONS or CIRCLE (suspended during Remap Test Mode and Tester Test Mode)
        if (!remapTestMode && !tester.testMode && ((justPressed & ORBIS_PAD_BUTTON_OPTIONS) || (justPressed & ORBIS_PAD_BUTTON_CIRCLE))) {
            break;
        }

        // Tab Switching via L1 / R1 (suspended during Remap Test Mode and Tester Test Mode)
        if (!remapTestMode && !tester.testMode) {
            if (justPressed & ORBIS_PAD_BUTTON_L1) {
                activeTabIndex = (activeTabIndex + numTabs - 1) % numTabs;
                currentTabId = activeTabs[activeTabIndex];
                if (currentTabId != TAB_TESTER && tester.rumblePhase != 0) {
                    tester.rumbleFrames = 0;
                    tester.rumblePhase = 0;
                    OrbisPadVibeParam vibe = { 0, 0 };
                    for (int i = 0; i < 4; i++) {
                        if (padHandles[i] >= 0) scePadSetVibration(padHandles[i], &vibe);
                    }
                    send_vibe_to_daemon(0, 0);
                }
            }
            if (justPressed & ORBIS_PAD_BUTTON_R1) {
                activeTabIndex = (activeTabIndex + 1) % numTabs;
                currentTabId = activeTabs[activeTabIndex];
                if (currentTabId != TAB_TESTER && tester.rumblePhase != 0) {
                    tester.rumbleFrames = 0;
                    tester.rumblePhase = 0;
                    OrbisPadVibeParam vibe = { 0, 0 };
                    for (int i = 0; i < 4; i++) {
                        if (padHandles[i] >= 0) scePadSetVibration(padHandles[i], &vibe);
                    }
                    send_vibe_to_daemon(0, 0);
                }
            }
        }

        if (currentTabId != TAB_REMAP_HID) {
            remapTestMode = false;
        }
        if (currentTabId != TAB_TESTER) {
            tester.testMode = false;
        }

        if (currentTabId == TAB_SETTINGS) {
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
                bool changed = false;
                if (selectedCard == 0) {
                    if (cfg.deadzone_index > 0) { cfg.deadzone_index--; changed = true; }
                } else if (selectedCard == 1) {
                    if (cfg.trigger_index > 0) { cfg.trigger_index--; changed = true; }
                } else if (selectedCard == 2) {
                    cfg.swap_index ^= 1; changed = true;
                } else if (selectedCard == 3) {
                    cfg.rumble_index ^= 1; changed = true;
                } else if (selectedCard == 4) {
                    cfg.switch_index ^= 1; changed = true;
                }
                if (changed) {
                    configMgr.save();
                    notify_daemon_reload();
                }
            }

            // Value changes (RIGHT)
            if (justPressed & ORBIS_PAD_BUTTON_RIGHT) {
                bool changed = false;
                if (selectedCard == 0) {
                    if (cfg.deadzone_index < (int)ConfigManager::DEADZONE_OPTIONS.size() - 1) {
                        cfg.deadzone_index++; changed = true;
                    }
                } else if (selectedCard == 1) {
                    if (cfg.trigger_index < (int)ConfigManager::TRIGGER_OPTIONS.size() - 1) {
                        cfg.trigger_index++; changed = true;
                    }
                } else if (selectedCard == 2) {
                    cfg.swap_index ^= 1; changed = true;
                } else if (selectedCard == 3) {
                    cfg.rumble_index ^= 1; changed = true;
                } else if (selectedCard == 4) {
                    cfg.switch_index ^= 1; changed = true;
                }
                if (changed) {
                    configMgr.save();
                    notify_daemon_reload();
                }
            }

            // Save on CROSS (X)
            if (justPressed & ORBIS_PAD_BUTTON_CROSS) {
                if (configMgr.save()) {
                    notify_daemon_reload();
                    toastText = "Settings saved & active!";
                    toastFrames = 180; // ~3 seconds toast feedback
                }
            }
        } else if (currentTabId == TAB_TESTER) {
            // --- TESTER & CALIBRATION TAB ---
            // R3 toggles between Normal Mode and isolated Test Mode
            if (justPressed & ORBIS_PAD_BUTTON_R3) {
                tester.testMode = !tester.testMode;
            }

            if (!tester.testMode) {
                // Left / Right / L2 / R2: Switch controller slot under test
                if ((justPressed & ORBIS_PAD_BUTTON_LEFT) || (justPressed & ORBIS_PAD_BUTTON_L2)) {
                    tester.selectedSlot = (tester.selectedSlot + 3) % 4;
                }
                if ((justPressed & ORBIS_PAD_BUTTON_RIGHT) || (justPressed & ORBIS_PAD_BUTTON_R2)) {
                    tester.selectedSlot = (tester.selectedSlot + 1) % 4;
                }

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
                int activeHandle = padHandles[tester.selectedSlot];
                if (activeHandle >= 0) scePadSetVibration(activeHandle, &vibe);
                send_vibe_to_daemon(vibe.lgMotor, vibe.smMotor);
            } else if (tester.rumblePhase != 0) {
                tester.rumblePhase = 0;
                OrbisPadVibeParam vibe = { 0, 0 };
                for (int i = 0; i < 4; i++) {
                    if (padHandles[i] >= 0) scePadSetVibration(padHandles[i], &vibe);
                }
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
        } else if (currentTabId == TAB_DUALSENSE && hasDualSense) {
            // --- DUALSENSE MANAGEMENT TAB ---
            const int TOTAL_DS_CARDS = 8;
            if (justPressed & ORBIS_PAD_BUTTON_UP) {
                dsSelectedCard = (dsSelectedCard + TOTAL_DS_CARDS - 1) % TOTAL_DS_CARDS;
            }
            if (justPressed & ORBIS_PAD_BUTTON_DOWN) {
                dsSelectedCard = (dsSelectedCard + 1) % TOTAL_DS_CARDS;
            }

            // Value changes (LEFT)
            if (justPressed & ORBIS_PAD_BUTTON_LEFT) {
                bool dsChanged = false;
                if (dsSelectedCard == 0) {
                    if (cfg.ds_lightbar_mode > 0) { cfg.ds_lightbar_mode--; dsChanged = true; }
                } else if (dsSelectedCard == 1) {
                    if (cfg.ds_lightbar_color > 0) { cfg.ds_lightbar_color--; dsChanged = true; }
                } else if (dsSelectedCard == 2) {
                    if (cfg.ds_brightness > 0) { cfg.ds_brightness--; dsChanged = true; }
                } else if (dsSelectedCard == 3) {
                    if (cfg.ds_trigger_mode > 0) { cfg.ds_trigger_mode--; dsChanged = true; }
                } else if (dsSelectedCard == 4) {
                    if (cfg.ds_player_leds > 0) { cfg.ds_player_leds--; dsChanged = true; }
                } else if (dsSelectedCard == 5) {
                    if (cfg.ds_speaker_volume >= 5) {
                        cfg.ds_speaker_volume -= 5;
                    } else {
                        cfg.ds_speaker_volume = 0;
                    }
                    dsChanged = true;
                } else if (dsSelectedCard == 6) {
                    cfg.ds_mute_action ^= 1; dsChanged = true;
                } else if (dsSelectedCard == 7) {
                    cfg.ds_bt_mode ^= 1; dsChanged = true;
                }
                if (dsChanged) {
                    configMgr.save();
                    notify_daemon_reload();
                }
            }

            // Value changes (RIGHT)
            if (justPressed & ORBIS_PAD_BUTTON_RIGHT) {
                bool dsChanged = false;
                if (dsSelectedCard == 0) {
                    if (cfg.ds_lightbar_mode < (int)ConfigManager::DS_LIGHTBAR_OPTIONS.size() - 1) {
                        cfg.ds_lightbar_mode++; dsChanged = true;
                    }
                } else if (dsSelectedCard == 1) {
                    if (cfg.ds_lightbar_color < (int)ConfigManager::DS_COLOR_OPTIONS.size() - 1) {
                        cfg.ds_lightbar_color++; dsChanged = true;
                    }
                } else if (dsSelectedCard == 2) {
                    if (cfg.ds_brightness < (int)ConfigManager::DS_BRIGHTNESS_OPTIONS.size() - 1) {
                        cfg.ds_brightness++; dsChanged = true;
                    }
                } else if (dsSelectedCard == 3) {
                    if (cfg.ds_trigger_mode < (int)ConfigManager::DS_TRIGGER_OPTIONS.size() - 1) {
                        cfg.ds_trigger_mode++; dsChanged = true;
                    }
                } else if (dsSelectedCard == 4) {
                    if (cfg.ds_player_leds < (int)ConfigManager::DS_PLAYER_LEDS_OPTIONS.size() - 1) {
                        cfg.ds_player_leds++; dsChanged = true;
                    }
                } else if (dsSelectedCard == 5) {
                    if (cfg.ds_speaker_volume <= 95) {
                        cfg.ds_speaker_volume += 5;
                    } else {
                        cfg.ds_speaker_volume = 100;
                    }
                    dsChanged = true;
                } else if (dsSelectedCard == 6) {
                    cfg.ds_mute_action ^= 1; dsChanged = true;
                } else if (dsSelectedCard == 7) {
                    cfg.ds_bt_mode ^= 1; dsChanged = true;
                }
                if (dsChanged) {
                    configMgr.save();
                    notify_daemon_reload();
                }
            }

            // Toggle Mute on SQUARE when on Speaker Volume card
            if ((justPressed & ORBIS_PAD_BUTTON_SQUARE) && dsSelectedCard == 5) {
                if (cfg.ds_speaker_volume > 0) {
                    cfg.ds_speaker_volume = 0;
                } else {
                    cfg.ds_speaker_volume = 80;
                }
                configMgr.save();
                notify_daemon_reload();
            }

            // Save on CROSS (X)
            if (justPressed & ORBIS_PAD_BUTTON_CROSS) {
                if (configMgr.save()) {
                    notify_daemon_reload();
                    toastText = "DualSense settings saved & active!";
                    toastFrames = 180;
                }
            }
        } else if (currentTabId == TAB_REMAP_HID && hasHid) {
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
                activeTabIndex = 0;
            }
        }

        // Auto-notify user as soon as an online update is detected
        UpdaterState curUpState = updater.get_state();
        if (lastUpdaterState != UPDATER_UPDATE_AVAILABLE && curUpState == UPDATER_UPDATE_AVAILABLE) {
            toastText = "Update available: " + updater.get_latest_version() + "! Press L1/R1 to visit Info";
            toastFrames = 240; // ~4s notification toast
        }
        lastUpdaterState = curUpState;

        if (toastFrames > 0) {
            toastFrames--;
        }

        // Render UI
        ui->render(scene, cfg, currentTabId, selectedCard, toastFrames, padData, tester, toastText.c_str(),
                   hasHid, &hidStatus, &hidProfile, remapSelectedRow, remapTestMode, &updater,
                   hasDualSense, dsSelectedCard);

        // Flip & VSync
        scene->SubmitFlip(frameID);
        scene->FrameWait(frameID);
        scene->FrameBufferSwap();
        frameID++;
    }

    for (int i = 0; i < 4; i++) {
        if (padHandles[i] >= 0) {
            OrbisPadVibeParam vibe = { 0, 0 };
            scePadSetVibration(padHandles[i], &vibe);
            scePadClose(padHandles[i]);
        }
    }
    send_vibe_to_daemon(0, 0);

    // Gracefully exit back to PS4 shell without CE-34878-0
    sceSystemServiceLoadExec("exit", NULL);
    exit(0);

    return 0;
}
