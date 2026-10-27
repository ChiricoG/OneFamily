#include "installer.h"
#include "config_manager.h"

#include <stdio.h>
#include <stdlib.h>
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

InstallResult Installer::runInstallation() {
    InstallResult res;

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

    // 3. Write payload to destination
    bool write1 = write_binary_file(TARGET_PAYLOAD_PATH, payloadData.data(), payloadData.size());
    bool write2 = write_binary_file(TARGET_USER_PAYLOAD_PATH, payloadData.data(), payloadData.size());
    res.payloadCopied = (write1 || write2);
    if (!res.payloadCopied) {
        res.detailMessage = "Failed to write payload to /data/payloads/onefamily.bin";
        return res;
    }

    // 4. Ensure default config exists
    ConfigManager cfgMgr;
    cfgMgr.load(); // automatically creates /data/GoldHEN/onefamily.ini if missing
    res.configCreated = true;

    // 5. Update /data/GoldHEN/payloads.ini for permanent autoboot
    res.autobootConfigured = update_payloads_ini();
    if (!res.autobootConfigured) {
        res.detailMessage = "Failed to configure /data/GoldHEN/payloads.ini";
        return res;
    }

    return res;
}
