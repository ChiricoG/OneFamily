#pragma once

#include <string>
#include <stdint.h>
#include <pthread.h>

enum UpdaterState {
    UPDATER_IDLE = 0,
    UPDATER_CHECKING,
    UPDATER_UP_TO_DATE,
    UPDATER_UPDATE_AVAILABLE,
    UPDATER_DOWNLOADING,
    UPDATER_DOWNLOAD_SUCCESS,
    UPDATER_ERROR
};

struct UpdaterProgress {
    uint64_t bytesReceived;
    uint64_t totalBytes;
    int percent;
};

class PkgUpdater {
public:
    static PkgUpdater& instance();

    bool init();
    void start_check();
    void start_download();

    UpdaterState get_state() const;
    UpdaterProgress get_progress() const;
    std::string get_latest_version() const;
    std::string get_status_message() const;
    std::string get_target_pkg_path() const;

private:
    PkgUpdater();
    ~PkgUpdater();

    static void* thread_entry(void* arg);
    void run();

    bool check_network_ready();
    bool query_latest_release(std::string& outTag, std::string& outPkgUrl, uint64_t& outPkgSize);
    bool download_file(const std::string& url, const std::string& destination, uint64_t expectedSize);

    bool is_version_newer(const std::string& remoteTag, const std::string& localVer);

    mutable pthread_mutex_t m_mutex;
    pthread_t m_thread;
    bool m_threadRunning;

    UpdaterState m_state;
    UpdaterProgress m_progress;

    std::string m_currentVersion;
    std::string m_latestVersion;
    std::string m_pkgDownloadUrl;
    uint64_t m_expectedPkgSize;
    std::string m_targetPkgPath;
    std::string m_statusMessage;

    bool m_actionDownloadTriggered;
};
