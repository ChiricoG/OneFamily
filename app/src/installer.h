#pragma once

#include <string>
#include <vector>
#include <stdint.h>

struct InstallResult {
    bool payloadCopied;
    bool configCreated;
    bool autobootConfigured;
    std::string detailMessage;

    InstallResult()
        : payloadCopied(false), configCreated(false),
          autobootConfigured(false), detailMessage("") {}

    bool isSuccess() const {
        return payloadCopied && configCreated && autobootConfigured;
    }
};

class Installer {
public:
    static bool isInstalled();
    static InstallResult runInstallation();
};
