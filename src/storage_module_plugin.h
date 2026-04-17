#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <logos_json.h>
#include <logos_result.h>

extern "C" {
#include "lib/libstorage.h"
}

class StorageModuleImpl {
public:
    StorageModuleImpl();
    ~StorageModuleImpl();

    // Wired automatically by the generated glue layer.
    // Call this to emit named events to other modules / the host application.
    // Data is a JSON-encoded string (object or array).
    std::function<void(const std::string& eventName, const std::string& data)> emitEvent;

    // Initialize the storage node with a JSON config string.
    //
    // Example config keys: data-dir, log-level, log-file, listen-addrs, bootstrap-node, ...
    // Returns true on success.
    // The method is synchronous.
    bool init(const std::string& cfg);

    // Start the storage node.
    // The method is asynchronous; emits "storageStart" event on completion.
    // Returns true if the start command was accepted.
    bool start();

    // Stop the storage node.
    // The method is asynchronous; emits "storageStop" event on completion.
    StdLogosResult stop();

    // Destroy the storage context and free all resources.
    // The node must be stopped before calling this.
    // The method is synchronous.
    StdLogosResult destroy();

    // Get the libstorage version string (does not require a started node).
    // The method is synchronous.
    StdLogosResult version();

    // Get the storage data directory.
    // The method is synchronous.
    StdLogosResult dataDir();

    // Get the node peer ID.
    // The method is synchronous.
    StdLogosResult peerId();

    // Get the node's Signed Peer Record (SPR).
    // The method is synchronous.
    StdLogosResult spr();

    // Get debug info: id, addrs, announceAddresses, table.
    // Returns a StdLogosResult whose value is a JSON object on success.
    // The method is synchronous.
    StdLogosResult debug();

    // Set the runtime log level (TRACE, DEBUG, INFO, NOTICE, WARN, ERROR, FATAL).
    // The method is synchronous.
    StdLogosResult updateLogLevel(const std::string& logLevel);

    // Connect to a peer by peer ID, optionally using explicit addresses.
    // The method is asynchronous; emits "storageConnect" event on completion.
    StdLogosResult connect(const std::string& peerId, const std::vector<std::string>& peerAddresses);

    // Upload a local file by path.
    // chunkSize controls the upload chunk size in bytes.
    // Returns a StdLogosResult with the session ID as value on success.
    // The method is asynchronous; emits "storageUploadProgress" and "storageUploadDone".
    StdLogosResult uploadUrl(const std::string& filePath, int64_t chunkSize);

    // Create an upload session for manual chunk-by-chunk upload.
    // Returns a StdLogosResult with the session ID as value on success.
    // The method is synchronous.
    StdLogosResult uploadInit(const std::string& filename, int64_t chunkSize);

    // Upload a single chunk for a session created with uploadInit.
    // Emits "storageUploadProgress".
    StdLogosResult uploadChunk(const std::string& sessionId, const std::string& chunk);

    // Finalize a manual upload session and retrieve the CID.
    // Returns a StdLogosResult with the CID as value on success.
    // The method is synchronous.
    StdLogosResult uploadFinalize(const std::string& sessionId);

    // Cancel an ongoing upload session.
    // The method is synchronous.
    StdLogosResult uploadCancel(const std::string& sessionId);

    // Download content by CID to a local file path.
    // If local=true, only uses locally cached data (no network fetch).
    // Returns a StdLogosResult with the session ID (= CID) as value on success.
    // The method is asynchronous; emits "storageDownloadProgress" and "storageDownloadDone".
    StdLogosResult downloadToUrl(const std::string& cid, const std::string& filePath,
                                  bool local, int64_t chunkSize);

    // 3-param wrapper for codegen compatibility (max 3 params in generated IPC)
    StdLogosResult downloadFile(const std::string& cid, const std::string& filePath,
                                bool local);

    // Download content by CID as a chunk stream.
    // Chunks are delivered via "storageDownloadProgress" events (chunk field in JSON).
    // Returns a StdLogosResult with the session ID (= CID) as value on success.
    // The method is asynchronous; emits "storageDownloadProgress" and "storageDownloadDone".
    StdLogosResult downloadChunks(const std::string& cid, bool local, int64_t chunkSize);

    // Cancel an ongoing download session.
    // The method is synchronous.
    StdLogosResult downloadCancel(const std::string& sessionId);

    // Check whether content identified by CID exists in local storage.
    // Returns a StdLogosResult with a bool value on success.
    // The method is synchronous.
    StdLogosResult exists(const std::string& cid);

    // Fetch content from the network and store it locally in the background.
    // The method is synchronous (accepts the request, download is async).
    StdLogosResult fetch(const std::string& cid);

    // Remove content identified by CID from local storage.
    // The method is synchronous.
    StdLogosResult remove(const std::string& cid);

    // Get storage space information.
    // Returns a StdLogosResult whose value is a JSON object on success:
    // totalBlocks, quotaMaxBytes, quotaUsedBytes, quotaReservedBytes.
    // The method is synchronous.
    StdLogosResult space();

    // List all manifests stored locally.
    // Returns a StdLogosResult whose value is a JSON array on success;
    // each item has: cid, treeCid, datasetSize, blockSize, filename, mimetype.
    // The method is synchronous.
    StdLogosResult manifests();

    // Download and return the manifest for a given CID.
    // Returns a StdLogosResult whose value is a JSON object on success:
    // cid, treeCid, datasetSize, blockSize, filename, mimetype.
    // The method is synchronous.
    StdLogosResult downloadManifest(const std::string& cid);

    // Import all files from a directory (headless helper).
    // Uploads each regular file found; does not wait for uploads to complete.
    void importFiles(const std::string& path);

    void emitEventSafe(const std::string& name, const std::string& data) const;

private:
    void* storageCtx;
    bool isStarted;

    // Shared internal download helper used by downloadToUrl and downloadChunks.
    // Returns session ID (= cid) on success, empty string on failure.
    std::string downloadChunksInternal(const std::string& cid,
                                       const std::string& filepath,
                                       bool local, int64_t chunkSize);
};
