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

    // If entry is already present, nothing to add
    if (content.find("onefamily.bin") != std::string::npos) {
        return true;
    }

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

    write_text_file(PAYLOADS_INI_PATH, content);
    write_text_file(PAYLOADS_INI_USER_PATH, content);
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

InstallResult Installer::runInstallation(bool isUpdate) {
    InstallResult res;
    res.isUpdate = isUpdate;

    // 1. Ensure target directory tree
    mkdir("/data", 0777);
    mkdir("/data/payloads", 0777);
    mkdir("/data/GoldHEN", 0777);
    mkdir("/user", 0777);
    mkdir("/user/data", 0777);
    mkdir("/user/data/payloads", 0777);
    mkdir("/user/data/GoldHEN", 0777);

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

    // 4. Overwrite / replace configuration file with latest version and default layout
    ConfigManager cfgMgr;
    cfgMgr.getConfig().set_defaults();
    cfgMgr.save(); // Overwrites /data/GoldHEN/onefamily.ini
    sceKernelUnlink(LEGACY_INI_FILE_PATH); // Clean up legacy xbox_controller.ini if present
    res.configCreated = true;

    // 5. Update /data/GoldHEN/payloads.ini for permanent autoboot (checked and skipped if already present)
    res.autobootConfigured = update_payloads_ini();
    if (!res.autobootConfigured) {
        res.detailMessage = "Failed to configure /data/GoldHEN/payloads.ini";
        return res;
    }

    return res;
}
