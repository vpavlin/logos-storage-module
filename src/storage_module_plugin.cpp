#include "storage_module_plugin.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Callback base — all context objects inherit from this.
// Only used for the async (event-emitting) dispatch path.
// ---------------------------------------------------------------------------

struct AsyncCallbackBase {
    virtual void handleResponse(int ret, const char* msg, size_t len) = 0;
    virtual ~AsyncCallbackBase() = default;
};

// Static callback for async contexts (start/stop/connect/upload progress/download).
// Ownership: each AsyncCallbackBase is heap-allocated and deleted here on non-PROGRESS.
static void asyncDispatch(int ret, const char* msg, size_t len, void* userData) {
    if (!userData) return;
    auto* base = static_cast<AsyncCallbackBase*>(userData);
    base->handleResponse(ret, msg, len);
    if (ret != RET_PROGRESS) {
        delete base;
    }
}

// ---------------------------------------------------------------------------
// SyncCtx — used for synchronous (blocking) libstorage calls.
//
// Lifetime rules:
//   - Allocated on the heap by the caller before issuing the command.
//   - Caller waits on the condvar, then checks ctx->received.
//   - If received == true before timeout: caller reads result and deletes ctx.
//   - If timeout fires before callback: caller marks ctx->abandoned = true
//     (under the same mutex) and does NOT delete; the callback will delete
//     when it eventually fires.
//
// This "abandoned" pattern prevents the use-after-free that would occur if
// libstorage calls the callback after the caller's stack frame has returned.
// ---------------------------------------------------------------------------

struct SyncCtx {
    std::mutex mtx;
    std::condition_variable cv;
    int resultCode = -1;
    std::string resultMsg;
    bool received = false;
    std::atomic<bool> abandoned{false};
    // Keeps the string argument alive across the (potentially async) C call.
    std::string lifetimeArg;

    SyncCtx() = default;
    SyncCtx(const SyncCtx&) = delete;
    SyncCtx& operator=(const SyncCtx&) = delete;
};

static void syncDispatch(int ret, const char* msg, size_t len, void* userData) {
    if (!userData) return;
    auto* ctx = static_cast<SyncCtx*>(userData);
    bool shouldDelete;
    {
        std::unique_lock<std::mutex> lock(ctx->mtx);
        ctx->resultCode = ret;
        ctx->resultMsg = (msg && len > 0) ? std::string(msg, len) : std::string();
        ctx->received = true;
        ctx->cv.notify_all();
        // Read abandoned while holding the lock so there is no race with the
        // caller's timeout path that also sets this flag under the lock.
        shouldDelete = ctx->abandoned.load();
    }
    if (shouldDelete) {
        delete ctx;
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string fromMsg(const char* msg, size_t len) {
    return (msg && len > 0) ? std::string(msg, len) : std::string();
}

struct SyncResult {
    bool ok = false;
    std::string message;
};

// Wait for a SyncCtx to be signalled (or time out).
// Returns the result and handles the abandoned-flag cleanup.
static SyncResult waitSync(SyncCtx* ctx, int timeoutMs) {
    SyncResult r;
    bool shouldDelete;
    {
        std::unique_lock<std::mutex> lock(ctx->mtx);
        ctx->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [ctx] { return ctx->received; });
        r.ok = ctx->received && ctx->resultCode == RET_OK;
        r.message = ctx->resultMsg;
        shouldDelete = ctx->received;
        if (!shouldDelete) {
            ctx->abandoned.store(true);
        }
    }
    if (shouldDelete) {
        delete ctx;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Concrete async context implementations
// ---------------------------------------------------------------------------

struct SimpleEventCtx : AsyncCallbackBase {
    StorageModuleImpl* impl;
    std::string eventName;
    // Tracks start/stop state changes on the impl.
    bool* isStartedFlag = nullptr;
    bool flagValueOnOk = false;

    SimpleEventCtx(StorageModuleImpl* i, std::string ev)
        : impl(i), eventName(std::move(ev)) {}

    void handleResponse(int ret, const char* msg, size_t len) override {
        if (isStartedFlag) {
            *isStartedFlag = (ret == RET_OK) ? flagValueOnOk : !flagValueOnOk;
        }
        std::string message = fromMsg(msg, len);
        try {
            json j;
            j["success"] = (ret == RET_OK);
            j["message"] = message;
            impl->emitEventSafe(eventName, j.dump());
        } catch (...) {}
    }
};

struct ConnectCtx : AsyncCallbackBase {
    StorageModuleImpl* impl;
    std::string peerIdBuf;
    std::vector<char*> addrs;

    ConnectCtx(StorageModuleImpl* i, std::string pid, std::vector<char*> a)
        : impl(i), peerIdBuf(std::move(pid)), addrs(std::move(a)) {}

    ~ConnectCtx() override {
        for (char* p : addrs) free(p);
    }

    void handleResponse(int ret, const char* msg, size_t len) override {
        std::string message = fromMsg(msg, len);
        try {
            json j;
            j["success"] = (ret == RET_OK);
            j["message"] = message;
            impl->emitEventSafe("storageConnect", j.dump());
        } catch (...) {}
    }
};

struct UploadFileCtx : AsyncCallbackBase {
    StorageModuleImpl* impl;
    std::string sessionId;
    int64_t totalBytes;
    mutable int64_t bytesUploaded = 0;
    mutable int64_t pendingBytes = 0;
    mutable int lastEmittedPercent = -1;

    UploadFileCtx(StorageModuleImpl* i, std::string sid, int64_t total)
        : impl(i), sessionId(std::move(sid)), totalBytes(total) {}

    void handleResponse(int ret, const char* msg, size_t len) override {
        if (ret == RET_PROGRESS) {
            bytesUploaded += static_cast<int64_t>(len);
            pendingBytes  += static_cast<int64_t>(len);
            if (totalBytes > 0) {
                int percent =
                    static_cast<int>((bytesUploaded * 100LL) / totalBytes);
                if (percent <= lastEmittedPercent) return;
                lastEmittedPercent = percent;
            }
            try {
                json j;
                j["success"] = true;
                j["sessionId"] = sessionId;
                j["bytes"] = pendingBytes;
                impl->emitEventSafe("storageUploadProgress", j.dump());
            } catch (...) {}
            pendingBytes = 0;
            return;
        }
        std::string message = fromMsg(msg, len);
        try {
            json j;
            j["success"] = (ret == RET_OK);
            j["sessionId"] = sessionId;
            if (ret == RET_OK) j["cid"] = message;
            else j["error"] = message;
            impl->emitEventSafe("storageUploadDone", j.dump());
        } catch (...) {}
    }
};

struct UploadChunkCtx : AsyncCallbackBase {
    StorageModuleImpl* impl;
    std::string sessionId;
    std::string chunk;

    UploadChunkCtx(StorageModuleImpl* i, std::string sid, std::string c)
        : impl(i), sessionId(std::move(sid)), chunk(std::move(c)) {}

    void handleResponse(int ret, const char* msg, size_t len) override {
        std::string message = fromMsg(msg, len);
        try {
            json j;
            j["success"] = (ret == RET_OK);
            j["sessionId"] = sessionId;
            if (ret == RET_OK) j["bytes"] = static_cast<int64_t>(chunk.size());
            else j["error"] = message;
            impl->emitEventSafe("storageUploadProgress", j.dump());
        } catch (...) {}
    }
};

struct DownloadStreamCtx : AsyncCallbackBase {
    StorageModuleImpl* impl;
    std::string cid;
    std::string filepath;
    int64_t totalBytes;
    mutable int64_t bytesDownloaded = 0;
    mutable int64_t pendingBytes = 0;
    mutable int lastEmittedPercent = -1;

    DownloadStreamCtx(StorageModuleImpl* i, std::string c, std::string fp,
                      int64_t total = 0)
        : impl(i), cid(std::move(c)), filepath(std::move(fp)),
          totalBytes(total) {}

    void handleResponse(int ret, const char* msg, size_t len) override {
        if (ret == RET_PROGRESS) {
            if (filepath.empty()) {
                // Chunk mode — copy the bytes and forward them.
                // A copy is required: the pointer is only valid during this call.
                std::string chunk(msg, len);
                try {
                    json j;
                    j["success"] = true;
                    j["sessionId"] = cid;
                    j["chunk"] = chunk;
                    impl->emitEventSafe("storageDownloadProgress", j.dump());
                } catch (...) {}
            } else {
                // File mode — report byte count, throttled.
                bytesDownloaded += static_cast<int64_t>(len);
                pendingBytes    += static_cast<int64_t>(len);
                if (totalBytes > 0) {
                    int percent = static_cast<int>(
                        (bytesDownloaded * 100LL) / totalBytes);
                    if (percent <= lastEmittedPercent) return;
                    lastEmittedPercent = percent;
                }
                try {
                    json j;
                    j["success"] = true;
                    j["sessionId"] = cid;
                    j["bytes"] = pendingBytes;
                    impl->emitEventSafe("storageDownloadProgress", j.dump());
                } catch (...) {}
                pendingBytes = 0;
            }
            return;
        }
        std::string message = fromMsg(msg, len);
        try {
            json j;
            j["success"] = (ret == RET_OK);
            j["sessionId"] = cid;
            if (ret != RET_OK) j["error"] = message;
            impl->emitEventSafe("storageDownloadDone", j.dump());
        } catch (...) {}
    }
};

// ---------------------------------------------------------------------------
// syncCall wrappers
// ---------------------------------------------------------------------------

using StorageNoArgFn = int (*)(void*, StorageCallback, void*);
using StorageStringFn = int (*)(void*, const char*, StorageCallback, void*);
using StorageStringIntFn = int (*)(void*, const char*, size_t, StorageCallback, void*);
using StorageDownloadInitFn =
    int (*)(void*, const char*, size_t, bool, StorageCallback, void*);

static SyncResult syncCallNoArg(void* ctx, StorageNoArgFn fn, int timeoutMs) {
    if (!ctx) return {false, "Storage context not initialized."};
    auto* sctx = new SyncCtx();
    if (fn(ctx, syncDispatch, sctx) != RET_OK) {
        delete sctx;
        return {false, "Failed to send command."};
    }
    return waitSync(sctx, timeoutMs);
}

static SyncResult syncCallString(void* ctx, StorageStringFn fn,
                                  const std::string& arg, int timeoutMs) {
    if (!ctx) return {false, "Storage context not initialized."};
    auto* sctx = new SyncCtx();
    sctx->lifetimeArg = arg;
    if (fn(ctx, sctx->lifetimeArg.c_str(), syncDispatch, sctx) != RET_OK) {
        delete sctx;
        return {false, "Failed to send command."};
    }
    return waitSync(sctx, timeoutMs);
}

static SyncResult syncCallStringSize(void* ctx, StorageStringIntFn fn,
                                      const std::string& arg, size_t n,
                                      int timeoutMs) {
    if (!ctx) return {false, "Storage context not initialized."};
    auto* sctx = new SyncCtx();
    sctx->lifetimeArg = arg;
    if (fn(ctx, sctx->lifetimeArg.c_str(), n, syncDispatch, sctx) != RET_OK) {
        delete sctx;
        return {false, "Failed to send command."};
    }
    return waitSync(sctx, timeoutMs);
}

static SyncResult syncCallDownloadInit(void* ctx, StorageDownloadInitFn fn,
                                        const std::string& cid, size_t chunkSize,
                                        bool local, int timeoutMs) {
    if (!ctx) return {false, "Storage context not initialized."};
    auto* sctx = new SyncCtx();
    sctx->lifetimeArg = cid;
    if (fn(ctx, sctx->lifetimeArg.c_str(), chunkSize, local, syncDispatch, sctx) !=
        RET_OK) {
        delete sctx;
        return {false, "Failed to send command."};
    }
    return waitSync(sctx, timeoutMs);
}

// ---------------------------------------------------------------------------
// StorageModuleImpl
// ---------------------------------------------------------------------------

StorageModuleImpl::StorageModuleImpl() : storageCtx(nullptr), isStarted(false) {
    fprintf(stderr, "StorageModuleImpl: Initializing...\n");
}

StorageModuleImpl::~StorageModuleImpl() {
    if (storageCtx) {
        fprintf(stderr,
                "StorageModuleImpl: Warning - storage context was not "
                "destroyed before plugin destruction\n");
        storageCtx = nullptr;
    }
}

void StorageModuleImpl::emitEventSafe(const std::string& name,
                                       const std::string& data) const {
    if (emitEvent) {
        emitEvent(name, data);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool StorageModuleImpl::init(const std::string& cfg) {
    fprintf(stderr, "StorageModuleImpl::init called\n");

    auto* sctx = new SyncCtx();
    storageCtx = storage_new(cfg.c_str(), syncDispatch, sctx);
    SyncResult r = waitSync(sctx, 1000);

    if (!r.ok || !storageCtx) {
        fprintf(stderr, "StorageModuleImpl::init failed: %s\n",
                r.message.c_str());
        storageCtx = nullptr;
        return false;
    }
    return true;
}

bool StorageModuleImpl::start() {
    fprintf(stderr, "StorageModuleImpl::start called\n");
    if (!storageCtx) {
        fprintf(stderr, "StorageModuleImpl::start: context not initialized\n");
        return false;
    }
    auto* ctx = new SimpleEventCtx(this, "storageStart");
    ctx->isStartedFlag = &isStarted;
    ctx->flagValueOnOk = true;
    if (storage_start(storageCtx, asyncDispatch, ctx) != RET_OK) {
        delete ctx;
        return false;
    }
    return true;
}

bool StorageModuleImpl::stop() {
    fprintf(stderr, "StorageModuleImpl::stop called\n");
    if (!storageCtx) {
        fprintf(stderr, "StorageModuleImpl::stop: context not initialized\n");
        return false;
    }
    auto* ctx = new SimpleEventCtx(this, "storageStop");
    ctx->isStartedFlag = &isStarted;
    ctx->flagValueOnOk = false;
    if (storage_stop(storageCtx, asyncDispatch, ctx) != RET_OK) {
        delete ctx;
        return false;
    }
    return true;
}

bool StorageModuleImpl::destroy() {
    fprintf(stderr, "StorageModuleImpl::destroy called\n");
    // Best-effort close (ignore errors).
    if (storageCtx) {
        syncCallNoArg(storageCtx, storage_close, 1000);
    }
    int ret = storage_destroy(storageCtx);
    if (ret == RET_OK) {
        storageCtx = nullptr;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

std::string StorageModuleImpl::version() {
    char* v = storage_version(storageCtx);
    if (!v) return {};
    std::string result(v);
    free(v);
    return result;
}

std::string StorageModuleImpl::dataDir() {
    auto r = syncCallNoArg(storageCtx, storage_repo, 1000);
    return r.ok ? r.message : std::string();
}

std::string StorageModuleImpl::peerId() {
    auto r = syncCallNoArg(storageCtx, storage_peer_id, 1000);
    return r.ok ? r.message : std::string();
}

std::string StorageModuleImpl::spr() {
    auto r = syncCallNoArg(storageCtx, storage_spr, 1000);
    return r.ok ? r.message : std::string();
}

LogosMap StorageModuleImpl::debug() {
    auto r = syncCallNoArg(storageCtx, storage_debug, 1000);
    if (!r.ok) return {};
    try {
        json doc = json::parse(r.message);
        LogosMap result;
        for (auto& [key, val] : doc.items()) {
            result[key] = val.dump();
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool StorageModuleImpl::updateLogLevel(const std::string& logLevel) {
    return syncCallString(storageCtx, storage_log_level, logLevel, 1000).ok;
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

bool StorageModuleImpl::connect(const std::string& peerId,
                                 const std::vector<std::string>& peerAddresses) {
    if (!storageCtx) return false;
    std::vector<char*> addrs;
    addrs.reserve(peerAddresses.size());
    for (const auto& a : peerAddresses) addrs.push_back(strdup(a.c_str()));

    auto* ctx = new ConnectCtx(this, peerId, addrs);
    if (storage_connect(storageCtx, ctx->peerIdBuf.c_str(),
                        const_cast<const char**>(ctx->addrs.data()),
                        ctx->addrs.size(), asyncDispatch, ctx) != RET_OK) {
        delete ctx;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

std::string StorageModuleImpl::uploadInit(const std::string& filename,
                                           int64_t chunkSize) {
    auto r = syncCallStringSize(storageCtx, storage_upload_init, filename,
                                static_cast<size_t>(chunkSize), 1000);
    return r.ok ? r.message : std::string();
}

std::string StorageModuleImpl::uploadUrl(const std::string& filePath,
                                          int64_t chunkSize) {
    fprintf(stderr, "StorageModuleImpl::uploadUrl called with path=%s\n",
            filePath.c_str());
    if (!storageCtx || chunkSize <= 0) return {};

    std::error_code ec;
    if (!fs::exists(filePath, ec) || !fs::is_regular_file(filePath, ec)) {
        fprintf(stderr, "StorageModuleImpl::uploadUrl: file not found or not regular: %s\n",
                filePath.c_str());
        return {};
    }

    int64_t fileSize = static_cast<int64_t>(fs::file_size(filePath, ec));
    std::string sessionId = uploadInit(filePath, chunkSize);
    if (sessionId.empty()) return {};

    auto* ctx = new UploadFileCtx(this, sessionId, fileSize);
    if (storage_upload_file(storageCtx, ctx->sessionId.c_str(),
                            asyncDispatch, ctx) != RET_OK) {
        delete ctx;
        uploadCancel(sessionId);
        return {};
    }
    return sessionId;
}

bool StorageModuleImpl::uploadChunk(const std::string& sessionId,
                                     const std::string& chunk) {
    if (!storageCtx) return false;
    auto* ctx = new UploadChunkCtx(this, sessionId, chunk);
    const auto* data = reinterpret_cast<const uint8_t*>(ctx->chunk.data());
    if (storage_upload_chunk(storageCtx, ctx->sessionId.c_str(), data,
                             ctx->chunk.size(), asyncDispatch, ctx) != RET_OK) {
        delete ctx;
        return false;
    }
    return true;
}

std::string StorageModuleImpl::uploadFinalize(const std::string& sessionId) {
    auto r = syncCallString(storageCtx, storage_upload_finalize, sessionId, 1000);
    return r.ok ? r.message : std::string();
}

bool StorageModuleImpl::uploadCancel(const std::string& sessionId) {
    return syncCallString(storageCtx, storage_upload_cancel, sessionId, 1000).ok;
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

std::string StorageModuleImpl::downloadChunksInternal(const std::string& cid,
                                                       const std::string& filepath,
                                                       bool local,
                                                       int64_t chunkSize) {
    if (!storageCtx || chunkSize <= 0) return {};

    // For file-mode download, get the manifest first so we can throttle progress
    // events to one per percentage point.
    int64_t totalBytes = 0;
    if (!filepath.empty()) {
        LogosMap manifest = downloadManifest(cid);
        if (manifest.empty()) {
            fprintf(stderr,
                    "StorageModuleImpl::downloadChunksInternal: failed to get "
                    "manifest for %s\n",
                    cid.c_str());
            return {};
        }
        if (manifest.contains("datasetSize") && !manifest["datasetSize"].is_null()) {
            const auto& ds = manifest["datasetSize"];
            try {
                if (ds.is_number_integer()) totalBytes = ds.get<int64_t>();
                else if (ds.is_number()) totalBytes = static_cast<int64_t>(ds.get<double>());
                else if (ds.is_string()) totalBytes = std::stoll(ds.get_ref<const std::string&>());
            } catch (...) {}
        }
    }

    // Init download session.
    auto r = syncCallDownloadInit(storageCtx, storage_download_init, cid,
                                  static_cast<size_t>(chunkSize), local, 1000);
    if (!r.ok) return {};

    // Start streaming.
    auto* ctx = new DownloadStreamCtx(this, cid, filepath, totalBytes);
    if (storage_download_stream(storageCtx, ctx->cid.c_str(),
                                static_cast<size_t>(chunkSize), local,
                                ctx->filepath.c_str(),
                                asyncDispatch, ctx) != RET_OK) {
        delete ctx;
        return {};
    }
    return cid;
}

std::string StorageModuleImpl::downloadToUrl(const std::string& cid,
                                              const std::string& filePath,
                                              bool local, int64_t chunkSize) {
    return downloadChunksInternal(cid, filePath, local, chunkSize);
}

std::string StorageModuleImpl::downloadChunks(const std::string& cid, bool local,
                                               int64_t chunkSize) {
    return downloadChunksInternal(cid, "", local, chunkSize);
}

bool StorageModuleImpl::downloadCancel(const std::string& sessionId) {
    return syncCallString(storageCtx, storage_download_cancel, sessionId, 1000).ok;
}

// ---------------------------------------------------------------------------
// Data management
// ---------------------------------------------------------------------------

bool StorageModuleImpl::exists(const std::string& cid) {
    auto r = syncCallString(storageCtx, storage_exists, cid, 1000);
    return r.ok && r.message == "true";
}

bool StorageModuleImpl::fetch(const std::string& cid) {
    return syncCallString(storageCtx, storage_fetch, cid, 3000).ok;
}

bool StorageModuleImpl::remove(const std::string& cid) {
    return syncCallString(storageCtx, storage_delete, cid, 3000).ok;
}

LogosMap StorageModuleImpl::space() {
    auto r = syncCallNoArg(storageCtx, storage_space, 1000);
    if (!r.ok) return {};
    try {
        json doc = json::parse(r.message);
        LogosMap result;
        for (auto& [key, val] : doc.items()) {
            result[key] = val.dump();
        }
        return result;
    } catch (...) {
        return {};
    }
}

LogosList StorageModuleImpl::manifests() {
    auto r = syncCallNoArg(storageCtx, storage_list, 1000);
    if (!r.ok) return {};
    try {
        json doc = json::parse(r.message);
        if (!doc.is_array()) return {};
        LogosList result = json::array();
        for (const auto& item : doc) {
            json mobj = item.value("manifest", json::object());
            LogosMap entry;
            entry["cid"]         = item.value("cid", "");
            entry["treeCid"]     = mobj.value("treeCid", "");
            entry["datasetSize"] = std::to_string(mobj.value("datasetSize", 0));
            entry["blockSize"]   = std::to_string(mobj.value("blockSize", 0));
            entry["filename"]    = mobj.value("filename", "");
            entry["mimetype"]    = mobj.value("mimetype", "");
            result.push_back(entry);
        }
        return result;
    } catch (...) {
        return {};
    }
}

LogosMap StorageModuleImpl::downloadManifest(const std::string& cid) {
    auto r = syncCallString(storageCtx, storage_download_manifest, cid, 3000);
    if (!r.ok) return {};
    try {
        json doc = json::parse(r.message);
        if (!doc.is_object()) return {};
        LogosMap result;
        for (auto& [key, val] : doc.items()) {
            result[key] = val.dump();
        }
        return result;
    } catch (...) {
        return {};
    }
}

// ---------------------------------------------------------------------------
// importFiles (headless helper)
// ---------------------------------------------------------------------------

void StorageModuleImpl::importFiles(const std::string& path) {
    fprintf(stderr, "StorageModuleImpl::importFiles from path=%s\n",
            path.c_str());
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        fprintf(stderr,
                "StorageModuleImpl::importFiles: not a directory: %s\n",
                path.c_str());
        return;
    }
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string fp = entry.path().string();
        fprintf(stderr, "StorageModuleImpl::importFiles: uploading %s\n",
                fp.c_str());
        std::string sid = uploadUrl(fp, 65536);
        if (sid.empty()) {
            fprintf(stderr,
                    "StorageModuleImpl::importFiles: failed to start upload "
                    "for %s\n",
                    fp.c_str());
        } else {
            fprintf(stderr,
                    "StorageModuleImpl::importFiles: upload started, "
                    "session=%s\n",
                    sid.c_str());
        }
    }
}
