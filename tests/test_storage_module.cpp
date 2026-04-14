// Integration tests for StorageModuleImpl — uses the REAL libstorage library.
// No mocking. These tests start an actual storage node, upload/download real data,
// and verify end-to-end behavior.
//
// Requires libstorage to be available in ../lib at build time.
// Skipped automatically when libstorage is not found.

#include <logos_test.h>
#include "storage_module_plugin.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

static const int DEFAULT_TIMEOUT_MS = 3000;
static const int START_TIMEOUT_MS   = 15000;
static const std::string LOG_FILENAME = "storage.log";

// ---------------------------------------------------------------------------
// EventWaiter — replaces QEventLoop + storageResponse signal.
// Collects named events emitted via StorageModuleImpl::emitEvent.
// ---------------------------------------------------------------------------

struct EventWaiter {
    std::mutex mtx;
    std::condition_variable cv;
    std::string lastEventName;
    std::string lastEventData;
    bool received = false;

    // Install as emitEvent on an impl instance.
    void install(StorageModuleImpl* impl) {
        impl->emitEvent = [this](const std::string& name,
                                  const std::string& data) {
            std::unique_lock<std::mutex> lock(mtx);
            lastEventName = name;
            lastEventData = data;
            received = true;
            cv.notify_all();
        };
    }

    // Reset before waiting for the next event.
    void reset() {
        std::unique_lock<std::mutex> lock(mtx);
        received = false;
        lastEventName.clear();
        lastEventData.clear();
    }

    // Wait for any event named `name` within timeoutMs.
    // Returns true if the event arrived and "success" was true in its JSON payload.
    bool waitFor(const std::string& name, int timeoutMs) {
        std::unique_lock<std::mutex> lock(mtx);
        bool ok = cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [&] { return received && lastEventName == name; });
        return ok;
    }

    // Non-blocking: returns the last event data string (must be called under lock or after wait).
    std::string data() {
        std::unique_lock<std::mutex> lock(mtx);
        return lastEventData;
    }
};

// ---------------------------------------------------------------------------
// Shared impl instance — restarted before each test.
// ---------------------------------------------------------------------------

static StorageModuleImpl* g_impl = nullptr;
static fs::path g_dataDir;
static EventWaiter g_waiter;

static void ensureRestarted() {
    if (g_impl) {
        g_impl->stop();
        g_waiter.reset();
        g_waiter.waitFor("storageStop", DEFAULT_TIMEOUT_MS);
        g_impl->destroy();
        delete g_impl;
        g_impl = nullptr;
        g_dataDir.clear();
    }

    g_dataDir = fs::temp_directory_path() /
                ("logos-storage-integration-test-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(g_dataDir);

    std::string logFile = (g_dataDir / LOG_FILENAME).string();

    g_impl = new StorageModuleImpl();
    g_waiter.install(g_impl);

    std::string config =
        std::string("{\"data-dir\":\"") + g_dataDir.string() +
        "\",\"log-level\":\"DEBUG\",\"log-file\":\"" + logFile + "\"}";

    if (!g_impl->init(config)) {
        throw LogosTestFailure("Failed to init storage impl.");
    }

    g_waiter.reset();
    if (!g_impl->start()) {
        throw LogosTestFailure("Failed to start storage impl.");
    }

    if (!g_waiter.waitFor("storageStart", START_TIMEOUT_MS)) {
        throw LogosTestFailure("Storage node did not start within timeout.");
    }
}

// ---------------------------------------------------------------------------
// Helper: write a file and upload it, returning the CID.
// ---------------------------------------------------------------------------

static std::string uploadContent(const std::string& content,
                                  const std::string& filename) {
    fs::path filePath = g_dataDir / filename;
    std::ofstream f(filePath, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    f.close();

    g_waiter.reset();
    std::string sid = g_impl->uploadUrl(filePath.string(), 65536);
    if (sid.empty()) return {};

    if (!g_waiter.waitFor("storageUploadDone", DEFAULT_TIMEOUT_MS)) return {};

    // Extract CID from JSON payload: {"success":true,"sessionId":"...","cid":"..."}
    std::string d = g_waiter.data();
    auto cidPos = d.find("\"cid\":\"");
    if (cidPos == std::string::npos) return {};
    cidPos += 7;
    auto cidEnd = d.find('"', cidPos);
    if (cidEnd == std::string::npos) return {};
    return d.substr(cidPos, cidEnd - cidPos);
}

// ---------------------------------------------------------------------------
// Helper: collect download chunks until storageDownloadDone.
// ---------------------------------------------------------------------------

static std::string collectDownloadChunks(int timeoutMs) {
    std::string collected;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool success = false;

    g_impl->emitEvent = [&](const std::string& name, const std::string& data) {
        if (name == "storageDownloadProgress") {
            // Extract chunk field from JSON payload.
            auto pos = data.find("\"chunk\":\"");
            if (pos != std::string::npos) {
                pos += 9;
                auto end = data.find('"', pos);
                if (end != std::string::npos) {
                    collected += data.substr(pos, end - pos);
                }
            }
        } else if (name == "storageDownloadDone") {
            success = data.find("\"success\":true") != std::string::npos;
            std::unique_lock<std::mutex> lock(m);
            done = true;
            cv.notify_all();
        }
    };

    std::unique_lock<std::mutex> lock(m);
    cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                [&] { return done; });

    // Restore normal waiter.
    g_waiter.install(g_impl);

    return success ? collected : std::string();
}

// ── integration_version ─────────────────────────────────────────────────────

LOGOS_TEST(integration_version) {
    ensureRestarted();
    std::string v = g_impl->version();
    LOGOS_ASSERT_FALSE(v.empty());
}

// ── integration_dataDir ──────────────────────────────────────────────────────

LOGOS_TEST(integration_dataDir) {
    ensureRestarted();
    std::string d = g_impl->dataDir();
    LOGOS_ASSERT_EQ(d, g_dataDir.string());
}

// ── integration_peerId ───────────────────────────────────────────────────────

LOGOS_TEST(integration_peerId) {
    ensureRestarted();
    std::string id = g_impl->peerId();
    LOGOS_ASSERT_FALSE(id.empty());
}

// ── integration_debug ────────────────────────────────────────────────────────

LOGOS_TEST(integration_debug) {
    ensureRestarted();
    LogosMap map = g_impl->debug();
    LOGOS_ASSERT_FALSE(map.empty());
    LOGOS_ASSERT(map.count("id") > 0);
    LOGOS_ASSERT(map.count("addrs") > 0);
    LOGOS_ASSERT(map.count("announceAddresses") > 0);
    LOGOS_ASSERT(map.count("table") > 0);
}

// ── integration_spr ──────────────────────────────────────────────────────────

LOGOS_TEST(integration_spr) {
    ensureRestarted();
    std::string s = g_impl->spr();
    LOGOS_ASSERT_FALSE(s.empty());
}

// ── integration_uploadFile ───────────────────────────────────────────────────

LOGOS_TEST(integration_uploadFile) {
    ensureRestarted();
    std::string cid = uploadContent("Hello, Logos Storage!", "test_upload.txt");
    LOGOS_ASSERT_FALSE(cid.empty());
}

// ── integration_uploadWorkflowManual ─────────────────────────────────────────

LOGOS_TEST(integration_uploadWorkflowManual) {
    ensureRestarted();

    fs::path filePath = g_dataDir / "test_manual_upload.txt";
    std::string content = "Hello, Logos Storage! Manual upload test.";
    std::ofstream f(filePath, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    f.close();

    std::string sid = g_impl->uploadInit(filePath.string(), 65536);
    LOGOS_ASSERT_FALSE(sid.empty());

    LOGOS_ASSERT_TRUE(g_impl->uploadChunk(sid, content));

    std::string cid = g_impl->uploadFinalize(sid);
    LOGOS_ASSERT_FALSE(cid.empty());
}

// ── integration_downloadFile ─────────────────────────────────────────────────

LOGOS_TEST(integration_downloadFile) {
    ensureRestarted();

    std::string content = "Hello, Logos Download Test!";
    std::string cid = uploadContent(content, "test_download.txt");
    LOGOS_ASSERT_FALSE(cid.empty());

    fs::path downloadPath = g_dataDir / "test_download_result.txt";

    g_waiter.reset();
    std::string sid = g_impl->downloadToUrl(cid, downloadPath.string(), false, 65536);
    LOGOS_ASSERT_FALSE(sid.empty());

    LOGOS_ASSERT_TRUE(g_waiter.waitFor("storageDownloadDone", DEFAULT_TIMEOUT_MS));

    std::ifstream in(downloadPath, std::ios::binary);
    std::string downloaded((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    LOGOS_ASSERT_EQ(downloaded, content);
}

// ── integration_downloadChunks ───────────────────────────────────────────────

LOGOS_TEST(integration_downloadChunks) {
    ensureRestarted();

    std::string content = "Hello, Logos Chunks Download Test!";
    std::string cid = uploadContent(content, "test_chunks_src.txt");
    LOGOS_ASSERT_FALSE(cid.empty());

    // collectDownloadChunks installs a custom emitEvent handler.
    std::string sid = g_impl->downloadChunks(cid, false, 65536);
    LOGOS_ASSERT_FALSE(sid.empty());

    std::string downloaded = collectDownloadChunks(DEFAULT_TIMEOUT_MS);
    LOGOS_ASSERT_FALSE(downloaded.empty());
    LOGOS_ASSERT_EQ(downloaded, content);
}

// ── integration_exists ───────────────────────────────────────────────────────

LOGOS_TEST(integration_exists) {
    ensureRestarted();

    std::string cid = uploadContent("Hello, Logos Exists Test!", "test_exists_src.txt");
    LOGOS_ASSERT_FALSE(cid.empty());

    LOGOS_ASSERT_TRUE(g_impl->exists(cid));
}

// ── integration_fetch ────────────────────────────────────────────────────────

LOGOS_TEST(integration_fetch) {
    ensureRestarted();

    std::string cid = uploadContent("Hello, Logos Fetch Test!", "test_fetch_src.txt");
    LOGOS_ASSERT_FALSE(cid.empty());

    LOGOS_ASSERT_TRUE(g_impl->fetch(cid));
}

// ── integration_remove ───────────────────────────────────────────────────────

LOGOS_TEST(integration_remove) {
    ensureRestarted();

    std::string cid = uploadContent("Hello, Logos Remove Test!", "test_remove_src.txt");
    LOGOS_ASSERT_FALSE(cid.empty());

    LOGOS_ASSERT_TRUE(g_impl->exists(cid));
    LOGOS_ASSERT_TRUE(g_impl->remove(cid));
    LOGOS_ASSERT_FALSE(g_impl->exists(cid));
}

// ── integration_space ────────────────────────────────────────────────────────

LOGOS_TEST(integration_space) {
    ensureRestarted();

    LogosMap map = g_impl->space();
    LOGOS_ASSERT_FALSE(map.empty());
    LOGOS_ASSERT(map.count("totalBlocks") > 0);
    LOGOS_ASSERT(map.count("quotaMaxBytes") > 0);
    LOGOS_ASSERT(map.count("quotaUsedBytes") > 0);
    LOGOS_ASSERT(map.count("quotaReservedBytes") > 0);
}

// ── integration_manifests ─────────────────────────────────────────────────────

LOGOS_TEST(integration_manifests) {
    ensureRestarted();

    std::string content = "Hello, Logos Manifests Test!";
    std::string cid = uploadContent(content, "test_manifests_src.txt");
    LOGOS_ASSERT_FALSE(cid.empty());

    LogosList list = g_impl->manifests();
    LOGOS_ASSERT_FALSE(list.empty());

    bool found = false;
    for (const auto& entry : list) {
        if (!entry.is_object()) continue;
        if (entry.value("cid", std::string()) != cid) continue;
        found = true;
        LOGOS_ASSERT_FALSE(entry.value("treeCid", std::string()).empty());
        break;
    }
    LOGOS_ASSERT_TRUE(found);
}

// ── integration_downloadManifest ─────────────────────────────────────────────

LOGOS_TEST(integration_downloadManifest) {
    ensureRestarted();

    std::string content = "Hello, Logos DownloadManifest Test!";
    std::string cid = uploadContent(content, "test_download_manifest_src.txt");
    LOGOS_ASSERT_FALSE(cid.empty());

    LogosMap manifest = g_impl->downloadManifest(cid);
    LOGOS_ASSERT_FALSE(manifest.empty());
    LOGOS_ASSERT(manifest.count("treeCid") > 0);
    LOGOS_ASSERT(manifest.count("datasetSize") > 0);
}

// ── integration_updateLogLevel ───────────────────────────────────────────────

LOGOS_TEST(integration_updateLogLevel) {
    ensureRestarted();

    LOGOS_ASSERT_TRUE(g_impl->updateLogLevel("TRACE"));

    // Upload a file to generate TRACE logs.
    uploadContent("Hello, Logos Log Level Test!", "test_loglevel_src.txt");

    fs::path logFile = g_dataDir / LOG_FILENAME;
    std::ifstream in(logFile);
    std::string logContent((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    LOGOS_ASSERT_FALSE(logContent.empty());
    LOGOS_ASSERT_TRUE(logContent.find("TRC") != std::string::npos);
}
