#include "pkg_updater.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Http.h>
#include <orbis/Sysmodule.h>

// Use custom typed wrappers to invoke symbols with their real parameters
extern "C" {
    int ofHttpSetAutoRedirect(int id, int enabled) __asm__("sceHttpSetAutoRedirect");
    int ofHttpSetRecvTimeOut(int id, uint32_t microseconds) __asm__("sceHttpSetRecvTimeOut");
}

#define GITHUB_API_LATEST_RELEASE "https://api.github.com/repos/ChiricoG/OneFamily/releases/latest"
#define HTTP_USER_AGENT "OneFamily-Companion/1.2.0-beta1 PS4"

#define ONEFAMILY_NET_POOLSIZE    (32 * 1024)
#define ONEFAMILY_HTTP_POOLSIZE   (128 * 1024)

static int s_netMemId = 0;
static int s_sslCtxId = 0;
static int s_httpCtxId = 0;
static bool s_httpInitialized = false;

static int skipSslCallback(int libsslId, unsigned int verifyErr, void* const sslCert[], int certNum, void* userArg) {
    (void)libsslId;
    (void)verifyErr;
    (void)sslCert;
    (void)certNum;
    (void)userArg;
    return 1; // Accept certificate
}

static bool ensure_http_stack() {
    if (s_httpInitialized) return true;

    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0) return false;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0) return false;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0) return false;

    int ret = sceNetInit();
    (void)ret;

    ret = sceNetPoolCreate("onefamily_net_pool", ONEFAMILY_NET_POOLSIZE, 0);
    if (ret < 0 && ret != (int)0x80410102) { // Already created check
        return false;
    }
    s_netMemId = ret;

    ret = sceSslInit(SSL_POOLSIZE);
    if (ret < 0 && ret != (int)0x80410102) {
        return false;
    }
    s_sslCtxId = ret;

    ret = sceHttpInit(s_netMemId, s_sslCtxId, ONEFAMILY_HTTP_POOLSIZE);
    if (ret < 0 && ret != (int)0x80410102) {
        return false;
    }
    s_httpCtxId = ret;

    s_httpInitialized = true;
    return true;
}

PkgUpdater& PkgUpdater::instance() {
    static PkgUpdater s_instance;
    return s_instance;
}

PkgUpdater::PkgUpdater()
    : m_threadRunning(false),
      m_state(UPDATER_IDLE),
      m_currentVersion("v1.2.0-beta1"),
      m_expectedPkgSize(0),
      m_targetPkgPath("/data/pkg/onefamily_companion.pkg"),
      m_actionDownloadTriggered(false) {
    pthread_mutex_init(&m_mutex, NULL);
    memset(&m_progress, 0, sizeof(m_progress));
}

PkgUpdater::~PkgUpdater() {
    pthread_mutex_destroy(&m_mutex);
}

bool PkgUpdater::init() {
    return true;
}

void PkgUpdater::start_check() {
    pthread_mutex_lock(&m_mutex);
    if (m_threadRunning) {
        pthread_mutex_unlock(&m_mutex);
        return;
    }
    m_state = UPDATER_CHECKING;
    m_statusMessage = "Checking for updates online...";
    m_actionDownloadTriggered = false;
    m_threadRunning = true;
    pthread_create(&m_thread, NULL, thread_entry, this);
    pthread_mutex_unlock(&m_mutex);
}

void PkgUpdater::start_download() {
    pthread_mutex_lock(&m_mutex);
    if (m_state == UPDATER_UPDATE_AVAILABLE) {
        m_actionDownloadTriggered = true;
    }
    pthread_mutex_unlock(&m_mutex);
}

UpdaterState PkgUpdater::get_state() const {
    pthread_mutex_lock(&m_mutex);
    UpdaterState st = m_state;
    pthread_mutex_unlock(&m_mutex);
    return st;
}

UpdaterProgress PkgUpdater::get_progress() const {
    pthread_mutex_lock(&m_mutex);
    UpdaterProgress pr = m_progress;
    pthread_mutex_unlock(&m_mutex);
    return pr;
}

std::string PkgUpdater::get_latest_version() const {
    pthread_mutex_lock(&m_mutex);
    std::string ver = m_latestVersion;
    pthread_mutex_unlock(&m_mutex);
    return ver;
}

std::string PkgUpdater::get_status_message() const {
    pthread_mutex_lock(&m_mutex);
    std::string msg = m_statusMessage;
    pthread_mutex_unlock(&m_mutex);
    return msg;
}

std::string PkgUpdater::get_target_pkg_path() const {
    return m_targetPkgPath;
}

void* PkgUpdater::thread_entry(void* arg) {
    PkgUpdater* updater = static_cast<PkgUpdater*>(arg);
    updater->run();
    return NULL;
}

bool PkgUpdater::check_network_ready() {
    int ret = sceNetCtlInit();
    if (ret < 0 && ret != (int)0x80410102) {
        return false;
    }

    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    ret = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (ret < 0 || info.ip_address[0] == '\0') {
        return false;
    }
    return true;
}

bool PkgUpdater::is_version_newer(const std::string& remoteTag, const std::string& localVer) {
    // Compare semantic versions (e.g. "v1.1.6" vs "v1.1.5")
    const char* r = remoteTag.c_str();
    const char* l = localVer.c_str();
    if (*r == 'v' || *r == 'V') r++;
    if (*l == 'v' || *l == 'V') l++;

    int rMajor = 0, rMinor = 0, rPatch = 0;
    int lMajor = 0, lMinor = 0, lPatch = 0;

    sscanf(r, "%d.%d.%d", &rMajor, &rMinor, &rPatch);
    sscanf(l, "%d.%d.%d", &lMajor, &lMinor, &lPatch);

    if (rMajor != lMajor) return rMajor > lMajor;
    if (rMinor != lMinor) return rMinor > lMinor;
    return rPatch > lPatch;
}

bool PkgUpdater::query_latest_release(std::string& outTag, std::string& outPkgUrl, uint64_t& outPkgSize) {
    if (!ensure_http_stack()) return false;

    int tpl = sceHttpCreateTemplate(s_httpCtxId, HTTP_USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) return false;

    sceHttpsSetSslCallback(tpl, skipSslCallback, NULL);
    ofHttpSetAutoRedirect(tpl, 1);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000); // 10s
    sceHttpSetSendTimeOut(tpl, 10 * 1000 * 1000);
    ofHttpSetRecvTimeOut(tpl, 10 * 1000 * 1000);

    int conn = sceHttpCreateConnectionWithURL(tpl, GITHUB_API_LATEST_RELEASE, 1);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, GITHUB_API_LATEST_RELEASE, 0);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    sceHttpAddRequestHeader(req, "Accept", "application/vnd.github.v3+json", 0);

    int ret = sceHttpSendRequest(req, NULL, 0);
    if (ret < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int statusCode = 0;
    sceHttpGetStatusCode(req, &statusCode);
    if (statusCode != 200) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    std::string jsonBody;
    char chunk[4096];
    while (true) {
        int read = sceHttpReadData(req, chunk, sizeof(chunk) - 1);
        if (read <= 0) break;
        chunk[read] = '\0';
        jsonBody.append(chunk, read);
    }

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);

    // Fast targeted JSON parsing for tag_name and onefamily_companion.pkg asset
    size_t tagPos = jsonBody.find("\"tag_name\"");
    if (tagPos == std::string::npos) return false;

    size_t tagStart = jsonBody.find('"', tagPos + 10);
    if (tagStart == std::string::npos) return false;
    size_t tagEnd = jsonBody.find('"', tagStart + 1);
    if (tagEnd == std::string::npos) return false;

    outTag = jsonBody.substr(tagStart + 1, tagEnd - tagStart - 1);

    // Look for "onefamily_companion.pkg" in assets
    size_t pkgPos = jsonBody.find("onefamily_companion.pkg");
    if (pkgPos == std::string::npos) return false;

    // Search for browser_download_url near pkgPos
    size_t urlKey = jsonBody.rfind("\"browser_download_url\"", pkgPos);
    if (urlKey == std::string::npos) {
        urlKey = jsonBody.find("\"browser_download_url\"", pkgPos);
    }
    if (urlKey == std::string::npos) return false;

    size_t urlStart = jsonBody.find('"', urlKey + 22);
    if (urlStart == std::string::npos) return false;
    size_t urlEnd = jsonBody.find('"', urlStart + 1);
    if (urlEnd == std::string::npos) return false;

    outPkgUrl = jsonBody.substr(urlStart + 1, urlEnd - urlStart - 1);

    // Search for size in bytes
    outPkgSize = 8500000; // sensible fallback ~8.5MB
    size_t sizeKey = jsonBody.find("\"size\"", pkgPos);
    if (sizeKey != std::string::npos && sizeKey < pkgPos + 300) {
        size_t numStart = jsonBody.find_first_of("0123456789", sizeKey + 6);
        if (numStart != std::string::npos) {
            outPkgSize = strtoull(&jsonBody[numStart], NULL, 10);
        }
    }

    return (!outTag.empty() && !outPkgUrl.empty());
}

bool PkgUpdater::download_file(const std::string& url, const std::string& destination, uint64_t expectedSize) {
    if (!ensure_http_stack()) return false;

    // Ensure /data and /data/pkg directories exist
    mkdir("/data", 0777);
    mkdir("/data/pkg", 0777);

    std::string tempDest = destination + ".download";

    int tpl = sceHttpCreateTemplate(s_httpCtxId, HTTP_USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) return false;

    sceHttpsSetSslCallback(tpl, skipSslCallback, NULL);
    ofHttpSetAutoRedirect(tpl, 1);
    sceHttpSetConnectTimeOut(tpl, 15 * 1000 * 1000); // 15s
    sceHttpSetSendTimeOut(tpl, 15 * 1000 * 1000);
    ofHttpSetRecvTimeOut(tpl, 30 * 1000 * 1000);

    int conn = sceHttpCreateConnectionWithURL(tpl, url.c_str(), 1);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int ret = sceHttpSendRequest(req, NULL, 0);
    if (ret < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int statusCode = 0;
    sceHttpGetStatusCode(req, &statusCode);
    if (statusCode != 200) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int contentLengthType = 0;
    uint64_t contentLength = 0;
    sceHttpGetResponseContentLength(req, &contentLengthType, &contentLength);
    if (contentLengthType == ORBIS_HTTP_CONTENTLEN_EXIST && contentLength > 0) {
        expectedSize = contentLength;
    }

    FILE* fp = fopen(tempDest.c_str(), "wb");
    if (!fp) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    uint8_t buffer[64 * 1024];
    uint64_t totalReceived = 0;

    pthread_mutex_lock(&m_mutex);
    m_progress.bytesReceived = 0;
    m_progress.totalBytes = expectedSize;
    m_progress.percent = 0;
    pthread_mutex_unlock(&m_mutex);

    bool downloadOk = true;
    while (true) {
        int bytesRead = sceHttpReadData(req, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            downloadOk = false;
            break;
        }
        if (bytesRead == 0) {
            // EOF reached
            break;
        }

        size_t written = fwrite(buffer, 1, bytesRead, fp);
        if (written != (size_t)bytesRead) {
            downloadOk = false;
            break;
        }

        totalReceived += bytesRead;

        pthread_mutex_lock(&m_mutex);
        m_progress.bytesReceived = totalReceived;
        if (expectedSize > 0) {
            m_progress.percent = (int)((totalReceived * 100) / expectedSize);
            if (m_progress.percent > 100) m_progress.percent = 100;
        }
        pthread_mutex_unlock(&m_mutex);
    }

    fclose(fp);

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);

    if (!downloadOk || totalReceived < 1024 * 1024) { // Minimum check (> 1MB for PKG)
        unlink(tempDest.c_str());
        return false;
    }

    // Atomic rename
    unlink(destination.c_str());
    if (rename(tempDest.c_str(), destination.c_str()) != 0) {
        return false;
    }

    return true;
}

void PkgUpdater::run() {
    // 1. Check network connectivity
    if (!check_network_ready()) {
        pthread_mutex_lock(&m_mutex);
        m_state = UPDATER_IDLE;
        m_statusMessage = "Offline (no network connection detected)";
        m_threadRunning = false;
        pthread_mutex_unlock(&m_mutex);
        return;
    }

    // 2. Query GitHub Releases API
    std::string tag;
    std::string pkgUrl;
    uint64_t pkgSize = 0;

    bool querySuccess = query_latest_release(tag, pkgUrl, pkgSize);
    if (!querySuccess) {
        pthread_mutex_lock(&m_mutex);
        m_state = UPDATER_ERROR;
        m_statusMessage = "Failed to query GitHub release";
        m_threadRunning = false;
        pthread_mutex_unlock(&m_mutex);
        return;
    }

    pthread_mutex_lock(&m_mutex);
    m_latestVersion = tag;
    m_pkgDownloadUrl = pkgUrl;
    m_expectedPkgSize = pkgSize;

    if (!is_version_newer(tag, m_currentVersion)) {
        m_state = UPDATER_UP_TO_DATE;
        m_statusMessage = "OneFamily Companion is up to date (" + m_currentVersion + ")";
        m_threadRunning = false;
        pthread_mutex_unlock(&m_mutex);
        return;
    }

    // Update available!
    m_state = UPDATER_UPDATE_AVAILABLE;
    m_statusMessage = "New update available: " + tag;
    pthread_mutex_unlock(&m_mutex);

    // Wait for user interaction (TRIANGLE press) or exit
    while (true) {
        pthread_mutex_lock(&m_mutex);
        bool trigger = m_actionDownloadTriggered;
        pthread_mutex_unlock(&m_mutex);

        if (trigger) break;
        usleep(100000); // 100ms
    }

    // 3. User requested download!
    pthread_mutex_lock(&m_mutex);
    m_state = UPDATER_DOWNLOADING;
    m_statusMessage = "Downloading " + tag + " PKG...";
    pthread_mutex_unlock(&m_mutex);

    bool dlOk = download_file(pkgUrl, m_targetPkgPath, pkgSize);

    pthread_mutex_lock(&m_mutex);
    if (dlOk) {
        m_state = UPDATER_DOWNLOAD_SUCCESS;
        m_statusMessage = "Package downloaded to /data/pkg/onefamily_companion.pkg!";
    } else {
        m_state = UPDATER_ERROR;
        m_statusMessage = "Download failed. Please check network/storage.";
    }
    m_threadRunning = false;
    pthread_mutex_unlock(&m_mutex);
}
