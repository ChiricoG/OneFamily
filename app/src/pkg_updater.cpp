#include "pkg_updater.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>

#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Http.h>
#include <orbis/Sysmodule.h>

#include <stdarg.h>
#include <time.h>

// Use custom typed wrappers to invoke symbols with their real parameters
extern "C" {
    int ofHttpSetAutoRedirect(int id, int enabled) __asm__("sceHttpSetAutoRedirect");
    int ofHttpSetRecvTimeOut(int id, uint32_t microseconds) __asm__("sceHttpSetRecvTimeOut");
    int ofHttpSetResponseHeaderMaxSize(int id, size_t size) __asm__("sceHttpSetResponseHeaderMaxSize");
}

#define GITHUB_API_RELEASES "https://api.github.com/repos/ChiricoG/OneFamily/releases"
#define HTTP_USER_AGENT "OneFamily-Companion/1.2.1 PS4"

#define ONEFAMILY_NET_POOLSIZE    (256 * 1024)
#define ONEFAMILY_HTTP_POOLSIZE   (1024 * 1024)

static void updater_log(const char* fmt, ...) {
    mkdir("/data", 0777);
    mkdir("/data/onefamily", 0777);
    FILE* f = fopen("/data/onefamily/updater.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

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
      m_currentVersion("v1.2.1"),
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

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024); // 512 KB stack
    pthread_create(&m_thread, &attr, thread_entry, this);
    pthread_attr_destroy(&attr);

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
    if (remoteTag == localVer) return false;

    const char* r = remoteTag.c_str();
    const char* l = localVer.c_str();
    if (*r == 'v' || *r == 'V') r++;
    if (*l == 'v' || *l == 'V') l++;

    int rMajor = 0, rMinor = 0, rPatch = 0, rBeta = 0;
    int lMajor = 0, lMinor = 0, lPatch = 0, lBeta = 0;

    sscanf(r, "%d.%d.%d", &rMajor, &rMinor, &rPatch);
    sscanf(l, "%d.%d.%d", &lMajor, &lMinor, &lPatch);

    const char* rBetaStr = strstr(r, "beta");
    if (rBetaStr) sscanf(rBetaStr, "beta%d", &rBeta);

    const char* lBetaStr = strstr(l, "beta");
    if (lBetaStr) sscanf(lBetaStr, "beta%d", &lBeta);

    if (rMajor != lMajor) return rMajor > lMajor;
    if (rMinor != lMinor) return rMinor > lMinor;
    if (rPatch != lPatch) return rPatch > lPatch;
    if (rBeta != lBeta) return rBeta > lBeta;

    return false;
}

bool PkgUpdater::query_latest_release(std::string& outTag, std::string& outPkgUrl, uint64_t& outPkgSize) {
    if (!ensure_http_stack()) return false;

    int tpl = sceHttpCreateTemplate(s_httpCtxId, HTTP_USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) return false;

    sceHttpsSetSslCallback(tpl, skipSslCallback, NULL);
    ofHttpSetAutoRedirect(tpl, 1);
    ofHttpSetResponseHeaderMaxSize(tpl, 64 * 1024);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000); // 10s
    sceHttpSetSendTimeOut(tpl, 10 * 1000 * 1000);
    ofHttpSetRecvTimeOut(tpl, 10 * 1000 * 1000);

    int conn = sceHttpCreateConnectionWithURL(tpl, GITHUB_API_RELEASES, 1);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, GITHUB_API_RELEASES, 0);
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

    // 1. Extract tag_name of the newest release (first entry in array)
    size_t tagPos = jsonBody.find("\"tag_name\"");
    if (tagPos == std::string::npos) return false;

    size_t tagStart = jsonBody.find('"', tagPos + 10);
    if (tagStart == std::string::npos) return false;
    size_t tagEnd = jsonBody.find('"', tagStart + 1);
    if (tagEnd == std::string::npos) return false;

    outTag = jsonBody.substr(tagStart + 1, tagEnd - tagStart - 1);

    // 2. Look for "onefamily_companion.pkg" in assets
    size_t pkgPos = jsonBody.find("\"onefamily_companion.pkg\"");
    if (pkgPos == std::string::npos) {
        pkgPos = jsonBody.find("onefamily_companion.pkg");
    }
    if (pkgPos == std::string::npos) return false;

    // Search for browser_download_url AFTER pkgPos to ensure we match onefamily_companion.pkg
    size_t urlKey = jsonBody.find("\"browser_download_url\"", pkgPos);
    if (urlKey == std::string::npos) return false;

    size_t urlStart = jsonBody.find('"', urlKey + 22);
    if (urlStart == std::string::npos) return false;
    size_t urlEnd = jsonBody.find('"', urlStart + 1);
    if (urlEnd == std::string::npos) return false;

    outPkgUrl = jsonBody.substr(urlStart + 1, urlEnd - urlStart - 1);

    // 3. Search for size in bytes near pkgPos
    outPkgSize = 8585216; // fallback ~8.5MB
    size_t sizeKey = jsonBody.find("\"size\"", pkgPos);
    if (sizeKey != std::string::npos && sizeKey < pkgPos + 500) {
        size_t numStart = jsonBody.find_first_of("0123456789", sizeKey + 6);
        if (numStart != std::string::npos) {
            outPkgSize = strtoull(&jsonBody[numStart], NULL, 10);
        }
    }

    return (!outTag.empty() && !outPkgUrl.empty());
}

bool PkgUpdater::download_file(const std::string& initialUrl, const std::string& destination, uint64_t expectedSize) {
    updater_log("\n========== [UPDATER] Starting download_file ==========\n");
    updater_log("[UPDATER] initialUrl: %s\n", initialUrl.c_str());
    updater_log("[UPDATER] destination: %s\n", destination.c_str());

    if (!ensure_http_stack()) {
        updater_log("[UPDATER] ensure_http_stack failed\n");
        return false;
    }

    // Ensure /data and /data/pkg directories exist
    mkdir("/data", 0777);
    mkdir("/data/pkg", 0777);

    std::string tempDest = destination + ".download";
    std::string currentUrl = initialUrl;

    const int MAX_REDIRECTS = 5;
    int redirects = 0;

    std::vector<uint8_t> buffer(64 * 1024);

    while (redirects < MAX_REDIRECTS) {
        int tpl = sceHttpCreateTemplate(s_httpCtxId, HTTP_USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
        if (tpl < 0) {
            updater_log("[UPDATER] sceHttpCreateTemplate failed: 0x%08X\n", (unsigned int)tpl);
            return false;
        }

        sceHttpsSetSslCallback(tpl, skipSslCallback, NULL);
        ofHttpSetAutoRedirect(tpl, 0); // Disable auto-redirect to handle cross-domain hops safely
        ofHttpSetResponseHeaderMaxSize(tpl, 64 * 1024);
        sceHttpSetConnectTimeOut(tpl, 15 * 1000 * 1000); // 15s
        sceHttpSetSendTimeOut(tpl, 15 * 1000 * 1000);
        ofHttpSetRecvTimeOut(tpl, 30 * 1000 * 1000);

        int conn = sceHttpCreateConnectionWithURL(tpl, currentUrl.c_str(), 1);
        if (conn < 0) {
            updater_log("[UPDATER] sceHttpCreateConnectionWithURL failed: 0x%08X (URL: %s)\n", (unsigned int)conn, currentUrl.c_str());
            sceHttpDeleteTemplate(tpl);
            return false;
        }

        int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, currentUrl.c_str(), 0);
        if (req < 0) {
            updater_log("[UPDATER] sceHttpCreateRequestWithURL failed: 0x%08X\n", (unsigned int)req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tpl);
            return false;
        }

        ofHttpSetResponseHeaderMaxSize(req, 64 * 1024);

        updater_log("[UPDATER] Sending GET [hop %d] to: %s\n", redirects, currentUrl.c_str());
        int ret = sceHttpSendRequest(req, NULL, 0);
        if (ret < 0) {
            updater_log("[UPDATER] sceHttpSendRequest failed: 0x%08X (%d)\n", (unsigned int)ret, ret);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tpl);
            return false;
        }

        int statusCode = 0;
        sceHttpGetStatusCode(req, &statusCode);
        updater_log("[UPDATER] HTTP statusCode: %d\n", statusCode);

        // Handle HTTP Redirects (301, 302, 303, 307, 308)
        if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
            char* headers = nullptr;
            size_t headerLen = 0;
            std::string nextUrl;

            int hRet = sceHttpGetAllResponseHeaders(req, &headers, &headerLen);
            updater_log("[UPDATER] sceHttpGetAllResponseHeaders ret=0x%08X, ptr=%p, len=%llu\n",
                        (unsigned int)hRet, headers, (unsigned long long)headerLen);

            if (hRet == 0 && headers && headerLen > 0) {
                std::string headerStr(headers, headerLen);
                size_t locPos = headerStr.find("Location:");
                if (locPos == std::string::npos) {
                    locPos = headerStr.find("location:");
                }
                if (locPos != std::string::npos) {
                    size_t valStart = locPos + 9;
                    while (valStart < headerStr.size() && (headerStr[valStart] == ' ' || headerStr[valStart] == '\t')) {
                        valStart++;
                    }
                    size_t valEnd = headerStr.find_first_of("\r\n", valStart);
                    if (valEnd != std::string::npos) {
                        nextUrl = headerStr.substr(valStart, valEnd - valStart);
                    } else {
                        nextUrl = headerStr.substr(valStart);
                    }
                }
            }

            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tpl);

            if (nextUrl.empty()) {
                updater_log("[UPDATER] ERROR: nextUrl is empty!\n");
                return false;
            }

            updater_log("[UPDATER] Redirect hop %d -> nextUrl: %s\n", redirects, nextUrl.c_str());
            currentUrl = nextUrl;
            redirects++;
            continue; // Re-issue GET to the redirect location with clean connection
        }

        if (statusCode != 200) {
            updater_log("[UPDATER] Non-200 HTTP status %d, aborting.\n", statusCode);
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tpl);
            return false;
        }

        // Status is 200 OK! Proceed with download stream
        int contentLengthType = 0;
        uint64_t contentLength = 0;
        sceHttpGetResponseContentLength(req, &contentLengthType, &contentLength);
        if (contentLengthType == ORBIS_HTTP_CONTENTLEN_EXIST && contentLength > 0) {
            expectedSize = contentLength;
        }
        updater_log("[UPDATER] 200 OK. ContentLength=%llu. Saving to %s\n", (unsigned long long)expectedSize, tempDest.c_str());

        FILE* fp = fopen(tempDest.c_str(), "wb");
        if (!fp) {
            updater_log("[UPDATER] fopen failed for %s\n", tempDest.c_str());
            sceHttpDeleteRequest(req);
            sceHttpDeleteConnection(conn);
            sceHttpDeleteTemplate(tpl);
            return false;
        }

        uint64_t totalReceived = 0;

        pthread_mutex_lock(&m_mutex);
        m_progress.bytesReceived = 0;
        m_progress.totalBytes = expectedSize;
        m_progress.percent = 0;
        pthread_mutex_unlock(&m_mutex);

        bool downloadOk = true;
        while (true) {
            int bytesRead = sceHttpReadData(req, buffer.data(), (uint32_t)buffer.size());
            if (bytesRead < 0) {
                updater_log("[UPDATER] sceHttpReadData error: 0x%08X (%d)\n", (unsigned int)bytesRead, bytesRead);
                downloadOk = false;
                break;
            }
            if (bytesRead == 0) {
                // EOF reached
                break;
            }

            size_t written = fwrite(buffer.data(), 1, bytesRead, fp);
            if (written != (size_t)bytesRead) {
                updater_log("[UPDATER] fwrite error: written %zu != %d\n", written, bytesRead);
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

        updater_log("[UPDATER] Download loop finished. totalReceived=%llu, downloadOk=%d\n",
                    (unsigned long long)totalReceived, (int)downloadOk);

        if (!downloadOk || totalReceived < 1024 * 1024) { // Minimum check (> 1MB for PKG)
            updater_log("[UPDATER] Validation failed (totalReceived=%llu < 1MB). Removing %s\n",
                        (unsigned long long)totalReceived, tempDest.c_str());
            unlink(tempDest.c_str());
            return false;
        }

        // Atomic rename
        unlink(destination.c_str());
        if (rename(tempDest.c_str(), destination.c_str()) != 0) {
            updater_log("[UPDATER] rename %s -> %s failed\n", tempDest.c_str(), destination.c_str());
            return false;
        }

        updater_log("[UPDATER] Successfully saved PKG to %s\n", destination.c_str());
        return true;
    }

    updater_log("[UPDATER] Max redirects reached!\n");
    return false;
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
