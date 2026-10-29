#include "installer.h"
#include "config_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <orbis/libkernel.h>

#define BUNDLED_PAYLOAD_PATH       "/app0/assets/payload/onefamily.bin"
#define BUNDLED_PAYLOAD_FALLBACK   "/app0/payload/onefamily.bin"

#define TARGET_PAYLOAD_PATH        "/data/payloads/onefamily.bin"
#define TARGET_USER_PAYLOAD_PATH   "/user/data/payloads/onefamily.bin"

#define PAYLOADS_INI_PATH          "/data/GoldHEN/payloads.ini"
#define PAYLOADS_INI_USER_PATH     "/user/data/GoldHEN/payloads.ini"

static bool file_exists(const char* path) {
    int fd = sceKernelOpen(path, O_RDONLY, 0);
    if (fd >= 0) {
        sceKernelClose(fd);
        return true;
    }
    return false;
}

static bool read_entire_file(const char* path, std::vector<uint8_t>& out) {
    out.clear();
    int fd = sceKernelOpen(path, O_RDONLY, 0);
    if (fd < 0) return false;

    off_t size = sceKernelLseek(fd, 0, SEEK_END);
    if (size <= 0) {
        sceKernelClose(fd);
        return false;
    }
    sceKernelLseek(fd, 0, SEEK_SET);

    out.resize((size_t)size);
    size_t total = 0;
    while (total < (size_t)size) {
        ssize_t rd = sceKernelRead(fd, out.data() + total, (size_t)size - total);
        if (rd <= 0) break;
        total += rd;
    }
    sceKernelClose(fd);
    return (total == (size_t)size);
}

static bool read_text_file(const char* path, std::string& out) {
    out.clear();
    std::vector<uint8_t> buf;
    if (!read_entire_file(path, buf)) return false;
    out.assign((const char*)buf.data(), buf.size());
    return true;
}

static bool write_text_file(const char* path, const std::string& content) {
    int fd = sceKernelOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    size_t total = 0;
    while (total < content.size()) {
        ssize_t wr = sceKernelWrite(fd, content.data() + total, content.size() - total);
        if (wr <= 0) break;
        total += wr;
    }
    sceKernelClose(fd);
    return (total == content.size());
}

static bool write_binary_file(const char* path, const void* data, size_t size) {
    int fd = sceKernelOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) return false;
    size_t total = 0;
    const uint8_t* ptr = (const uint8_t*)data;
    while (total < size) {
        ssize_t wr = sceKernelWrite(fd, ptr + total, size - total);
        if (wr <= 0) break;
        total += wr;
    }
    sceKernelClose(fd);
    return (total == size);
}

static bool update_payloads_ini() {
    std::string content;
    bool found = read_text_file(PAYLOADS_INI_PATH, content);
    if (!found) {
        found = read_text_file(PAYLOADS_INI_USER_PATH, content);
    }

    const std::string entryLine = "/user/data/payloads/onefamily.bin = 1";

    if (!found || content.empty()) {
        // File does not exist yet; create it with default [AutoRun] section
        std::string newIni = "[AutoRun]\n" + entryLine + "\n";
        write_text_file(PAYLOADS_INI_PATH, newIni);
        write_text_file(PAYLOADS_INI_USER_PATH, newIni);
        return true;
    }

    bool modified = false;

    // Remove any obsolete legacy xbox_controller.bin autorun lines
    size_t xpos = 0;
    while ((xpos = content.find("xbox_controller.bin")) != std::string::npos) {
        size_t lineStart = content.rfind('\n', xpos);
        if (lineStart == std::string::npos) lineStart = 0;
        else lineStart++; // move past '\n'
        size_t lineEnd = content.find('\n', xpos);
        if (lineEnd == std::string::npos) {
            content.erase(lineStart);
        } else {
            content.erase(lineStart, lineEnd - lineStart + 1);
        }
        modified = true;
    }

    // Ensure onefamily.bin is present in [AutoRun]
    if (content.find("onefamily.bin") == std::string::npos) {
        // Find [AutoRun] section (case-insensitive)
        std::string lower = content;
        for (size_t i = 0; i < lower.size(); i++) {
            lower[i] = (char)::tolower((unsigned char)lower[i]);
        }

        size_t pos = lower.find("[autorun]");
        if (pos != std::string::npos) {
            // Insert right after the [AutoRun] line
            size_t eol = content.find('\n', pos);
            if (eol == std::string::npos) {
                content += "\n" + entryLine + "\n";
            } else {
                content.insert(eol + 1, entryLine + "\n");
            }
        } else {
            // Section not present; append to end
            if (!content.empty() && content.back() != '\n') {
                content += "\n";
            }
            content += "\n[AutoRun]\n" + entryLine + "\n";
        }
        modified = true;
    }

    if (modified) {
        write_text_file(PAYLOADS_INI_PATH, content);
        write_text_file(PAYLOADS_INI_USER_PATH, content);
    }
    return true;
}

bool Installer::isPayloadUpToDate() {
    // 1. Locate bundled payload
    const char* bundled_path = NULL;
    if (file_exists(BUNDLED_PAYLOAD_PATH)) {
        bundled_path = BUNDLED_PAYLOAD_PATH;
    } else if (file_exists(BUNDLED_PAYLOAD_FALLBACK)) {
        bundled_path = BUNDLED_PAYLOAD_FALLBACK;
    }
    if (!bundled_path) return false;

    // 2. Locate installed target payload
    const char* target_path = NULL;
    if (file_exists(TARGET_PAYLOAD_PATH)) {
        target_path = TARGET_PAYLOAD_PATH;
    } else if (file_exists(TARGET_USER_PAYLOAD_PATH)) {
        target_path = TARGET_USER_PAYLOAD_PATH;
    }
    if (!target_path) return false;

    // 3. Open both and compare file sizes
    int fd_b = sceKernelOpen(bundled_path, O_RDONLY, 0);
    if (fd_b < 0) return false;
    off_t bundled_sz = sceKernelLseek(fd_b, 0, SEEK_END);
    sceKernelLseek(fd_b, 0, SEEK_SET);

    int fd_t = sceKernelOpen(target_path, O_RDONLY, 0);
    if (fd_t < 0) {
        sceKernelClose(fd_b);
        return false;
    }
    off_t target_sz = sceKernelLseek(fd_t, 0, SEEK_END);
    sceKernelLseek(fd_t, 0, SEEK_SET);

    if (bundled_sz <= 0 || target_sz <= 0 || bundled_sz != target_sz) {
        sceKernelClose(fd_b);
        sceKernelClose(fd_t);
        return false;
    }

    // 4. Compare contents in 8 KB chunks
    uint8_t buf_b[8192];
    uint8_t buf_t[8192];
    bool match = true;
    off_t remaining = bundled_sz;

    while (remaining > 0) {
        size_t chunk = (remaining > (off_t)sizeof(buf_b)) ? sizeof(buf_b) : (size_t)remaining;
        ssize_t rb = sceKernelRead(fd_b, buf_b, chunk);
        ssize_t rt = sceKernelRead(fd_t, buf_t, chunk);
        if (rb != (ssize_t)chunk || rt != (ssize_t)chunk || memcmp(buf_b, buf_t, chunk) != 0) {
            match = false;
            break;
        }
        remaining -= chunk;
    }

    sceKernelClose(fd_b);
    sceKernelClose(fd_t);
    return match;
}

bool Installer::isInstalled() {
    // 1. Target payload must exist
    bool binExists = file_exists(TARGET_PAYLOAD_PATH) || file_exists(TARGET_USER_PAYLOAD_PATH);
    if (!binExists) return false;

    // 2. payloads.ini must exist and mention onefamily.bin
    std::string content;
    bool iniExists = read_text_file(PAYLOADS_INI_PATH, content);
    if (!iniExists) {
        iniExists = read_text_file(PAYLOADS_INI_USER_PATH, content);
    }
    if (!iniExists) return false;

    if (content.find("onefamily.bin") == std::string::npos) {
        return false;
    }

    return true;
}

static const struct {
    const char* filename;
    const char* content;
} k_factory_profiles[] = {
    {
        "default.ini",
        "# OneFamily Generic Controller Profile - Default Template\n"
        "[Info]\n"
        "name = Generic Controller (Default)\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = square\n"
        "button4 = triangle\n"
        "button5 = l1\n"
        "button6 = r1\n"
        "button7 = l2\n"
        "button8 = r2\n"
        "button9 = create\n"
        "button10 = options\n"
        "button11 = l3\n"
        "button12 = r3\n"
        "button13 = ps\n"
        "button14 = touchpad\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    },
    {
        "2DC8_310B.ini",
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo Ultimate 2 Wireless\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x310B\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    },
    {
        "2DC8_3012.ini",
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo Ultimate 2.4G\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x3012\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    },
    {
        "2DC8_6012.ini",
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo Ultimate 2C Wireless\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x6012\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    },
    {
        "2DC8_2100.ini",
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = 8BitDo SN30 Pro\n"
        "vendor_id = 0x2DC8\n"
        "product_id = 0x2100\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    },
    {
        "3537_1022.ini",
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = GameSir G7\n"
        "vendor_id = 0x3537\n"
        "product_id = 0x1022\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    },
    {
        "FFFF_046E.ini",
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = GameSir G3s\n"
        "vendor_id = 0xFFFF\n"
        "product_id = 0x046E\n"
        "\n"
        "[Buttons]\n"
        "button1 = cross\n"
        "button2 = circle\n"
        "button3 = none\n"
        "button4 = square\n"
        "button5 = triangle\n"
        "button6 = none\n"
        "button7 = l1\n"
        "button8 = r1\n"
        "button9 = l2\n"
        "button10 = r2\n"
        "button11 = create\n"
        "button12 = options\n"
        "button13 = ps\n"
        "button14 = l3\n"
        "button15 = r3\n"
        "\n"
        "[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n"
        "[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    }
};

static void install_controller_profiles() {
    for (size_t i = 0; i < sizeof(k_factory_profiles) / sizeof(k_factory_profiles[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path), "/data/onefamily/controllers/%s", k_factory_profiles[i].filename);
        // If default.ini already exists on disk, do not overwrite user modifications
        if (strcmp(k_factory_profiles[i].filename, "default.ini") != 0 || !file_exists(path)) {
            write_text_file(path, k_factory_profiles[i].content);
        }

        snprintf(path, sizeof(path), "/user/data/onefamily/controllers/%s", k_factory_profiles[i].filename);
        if (strcmp(k_factory_profiles[i].filename, "default.ini") != 0 || !file_exists(path)) {
            write_text_file(path, k_factory_profiles[i].content);
        }
    }
}

InstallResult Installer::runInstallation(bool isUpdate) {
    InstallResult res;
    res.isUpdate = isUpdate;

    // 1. Ensure target directory tree
    mkdir("/data", 0777);
    mkdir("/data/payloads", 0777);
    mkdir("/data/GoldHEN", 0777);
    mkdir("/data/onefamily", 0777);
    mkdir("/data/onefamily/controllers", 0777);
    mkdir("/user", 0777);
    mkdir("/user/data", 0777);
    mkdir("/user/data/payloads", 0777);
    mkdir("/user/data/GoldHEN", 0777);
    mkdir("/user/data/onefamily", 0777);
    mkdir("/user/data/onefamily/controllers", 0777);

    // 2. Read bundled payload from PKG
    std::vector<uint8_t> payloadData;
    bool readOk = read_entire_file(BUNDLED_PAYLOAD_PATH, payloadData);
    if (!readOk) {
        readOk = read_entire_file(BUNDLED_PAYLOAD_FALLBACK, payloadData);
    }

    if (!readOk || payloadData.empty()) {
        res.detailMessage = "Could not locate bundled onefamily.bin in /app0/assets/payload/";
        return res;
    }

    // 3. Write payload to destination (overwriting existing file with latest version)
    bool write1 = write_binary_file(TARGET_PAYLOAD_PATH, payloadData.data(), payloadData.size());
    bool write2 = write_binary_file(TARGET_USER_PAYLOAD_PATH, payloadData.data(), payloadData.size());
    res.payloadCopied = (write1 || write2);
    if (!res.payloadCopied) {
        res.detailMessage = "Failed to write payload to /data/payloads/onefamily.bin";
        return res;
    }

    // 4. Migrate or initialize configuration file with user preferences preserved
    ConfigManager cfgMgr;
    cfgMgr.load(); // Loads from /data/onefamily/onefamily.ini, or migrates from GoldHEN legacy paths, or sets defaults
    cfgMgr.save(); // Writes to /data/onefamily/onefamily.ini

    // Install default generic controller profiles in /data/onefamily/controllers/
    install_controller_profiles();

    // Clean up legacy files and orphan binaries if present
    sceKernelUnlink("/data/payloads/xbox_controller.bin");
    sceKernelUnlink("/user/data/payloads/xbox_controller.bin");
    sceKernelUnlink(LEGACY_INI_FILE_PATH);      // /data/GoldHEN/onefamily.ini
    sceKernelUnlink(OLD_LEGACY_INI_FILE_PATH);  // /data/GoldHEN/xbox_controller.ini
    sceKernelUnlink("/user/data/GoldHEN/onefamily.ini");
    sceKernelUnlink("/user/data/GoldHEN/xbox_controller.ini");
    sceKernelUnlink("/data/GoldHEN/onefamily.log");
    sceKernelUnlink("/data/GoldHEN/xbox_controller.log");
    sceKernelUnlink("/user/data/GoldHEN/onefamily.log");
    sceKernelUnlink("/user/data/GoldHEN/xbox_controller.log");
    sceKernelUnlink("/data/GoldHEN/onefamily.vibe");
    sceKernelUnlink("/user/data/GoldHEN/onefamily.vibe");
    rmdir("/data/onefamily/maps");
    rmdir("/user/data/onefamily/maps");
    res.configCreated = true;

    // 5. Update /data/GoldHEN/payloads.ini for permanent autoboot (checked and skipped if already present)
    res.autobootConfigured = update_payloads_ini();
    if (!res.autobootConfigured) {
        res.detailMessage = "Failed to configure /data/GoldHEN/payloads.ini";
        return res;
    }

    return res;
}
