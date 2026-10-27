#pragma once

#include <string>
#include <vector>
#include <stdint.h>

struct InstallResult {
    bool isUpdate;
    bool payloadCopied;
    bool configCreated;
    bool autobootConfigured;
    std::string detailMessage;

    InstallResult()
        : isUpdate(false), payloadCopied(false), configCreated(false),
          autobootConfigured(false), detailMessage("") {}

    bool isSuccess() const {
        return payloadCopied && configCreated && autobootConfigured;
    }
};

class Installer {
public:
    static bool isInstalled();
    static bool isPayloadUpToDate();
    static InstallResult runInstallation(bool isUpdate = false);
};
