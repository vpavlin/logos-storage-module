# logos-storage-module

## How to Build

### Using Nix

#### Build Complete Module (Library + Headers)

```bash
# Build everything (default)
nix build

# Or explicitly
nix build '.#default'
```

The result will include:
- `/lib/storage_module_plugin.dylib` (or `.so` on Linux) - The Storage module plugin

#### Build Individual Components

```bash
# Build only the library (plugin + libstorage)
nix build '.#lib'

# Build only the generated headers
nix build '.#include'
```

#### Development Shell

```bash
# Enter development shell with all dependencies
nix develop
```

**Note:** In zsh, you need to quote the target (e.g., `'.#default'`) to prevent glob expansion.

If you don't have flakes enabled globally, add experimental flags:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

To enable globally so you don't need these flag for each command, add the following to `~/.config/nix/nix.conf` (create if it doesn't exist):
```ini
experimental-features = nix-command flakes
```

The compiled artifacts can be found at `result/`

#### Headless mode

The headless mode can be run from any directory, not just the Storage Module root. The following commands assume you're in the directory where you want to run the headless mode.

First, retrieve lib-logos:

```bash
nix --extra-experimental-features "nix-command flakes" build github:logos-co/logos-liblogos --out-link ./logos
```

Create the modules directory:

```bash
mkdir modules
```

Now retrieve the library files. You have two options:

Using the package manager:

```bash
nix --extra-experimental-features "nix-command flakes" build github:logos-co/logos-package-manager-module#cli --out-link ./package-manager
./package-manager/bin/lgpm --modules-dir ./modules/ install logos-storage-module
```

Or copy from a local build:

```bash
cp /path/to/logos-storage-module/result/lib/* modules/
```

Get the configuration file, either from the repository or use a local copy:

```bash
# Download from repository
wget https://raw.githubusercontent.com/logos-co/node-configs/refs/heads/master/storage_config.json

# Or copy local file
cp /path/to/config.json .
```

Run the headless mode:

```bash
./logos/bin/logoscore -m ./modules --load-modules storage_module -c "storage_module.init(@config.json)" -c "storage_module.start()" -c "storage_module.importFiles(/path/to/import/files)"
```

This command does three things: initialize the module from `config.json`, start the node, and import files from the specified directory.

You should see logs similar to:

```
Debug: [LOGOS_HOST "storage_module" ]: "LogosAPIClient: Received event: \"storageUploadDone\""
Debug: [LOGOS_HOST "storage_module" ]: "LogosAPIClient: Emitting event: \"storageUploadDone\""
Debug: [LOGOS_HOST "storage_module" ]: "File \"CMakeLists.txt\" uploaded successfully, session: \"0\" cid= \"zDvZRwzkyHVgr59zFkX7vyfzK7oUP7Jc6k7qpFD9ssDi7V5fvdjw\""
Debug: [LOGOS_HOST "storage_module" ]: "importFiles completed: 1 / 1 files uploaded"
```

#### SELinux

If you are using Linux with SELinux enabled, you will not be able to install Nix without disabling it. A common workaround is to install Nix inside a Toolbox container.

#### Modular Architecture

The build system is handled by `logos-module-builder`. This module uses the **universal** interface (`"interface": "universal"` in `metadata.json`), which means any glue is auto-generated at build time from `src/storage_module_plugin.h` (via `codegen.impl_header` in `metadata.json`) by `logos-cpp-generator`.

## Output Structure

When built with Nix, the module produces:

```
result/
└── lib/
    └── storage_module_plugin.dylib  # Logos module plugin
```

Both libraries must remain in the same directory, as `storage_module_plugin.dylib` is configured with `@loader_path` to find `libstorage.dylib` relative to itself.

## API

This tutorial will explain how to use the API for basic operations, i.e., upload and download operations.

This tutorial assumes that you already have the [sdk](https://github.com/logos-co/logos-cpp-sdk) and [logos core](https://github.com/logos-co/logos-liblogos). Please refer to the respective documentations to setup your project.

`m_logos` refers to a `LogosModules` instance that is supposed to be already created.

The API has been designed to work with the Logos SDK architecture. This module uses the **universal** interface — the implementation is plain C++ (`StorageModuleImpl`).

`Logos Storage` refers to the [nim project](https://github.com/logos-storage/logos-storage-nim) hosting the actual code of the Storage engine.

`Logos Storage Module` refers to this project.

#### Return types

Methods return standard C++ types (`bool`, `std::string`, `LogosMap`, `LogosList`). Via the generated `LogosModules` wrapper, these are exposed as `QVariant`-based types following the SDK conventions.

Async operations return a session ID (`std::string`) or `bool` to confirm the command was accepted. Completion and progress are delivered through named events.

#### Events

All events carry a JSON-encoded payload string. Parse it with `QJsonDocument` or pass it to `nlohmann::json`. Common fields:

| Field | Type | Description |
|---|---|---|
| `success` | bool | Whether the operation succeeded |
| `message` | string | Error description on failure |
| `sessionId` | string | Session identifier (uploads, downloads) |
| `cid` | string | Content identifier (on upload done) |
| `bytes` | number | Bytes transferred (progress events) |
| `chunk` | string | Raw data bytes (chunk-mode download) |

#### Init

Before using the Logos Storage Module you need to initialize it by calling the function `init`:

```cpp
const std::string jsonConfig = "{}";
bool ok = m_logos->storage_module.init(jsonConfig);
```

You can check the possible config keys in `src/storage_module_plugin.h`.

**Important note**: Do not call `init` more than once per instance.

#### Lifecycle

Start the storage node:

```cpp
bool ok = m_logos->storage_module.start();
```

`start()` returns `true` if the command was accepted. Actual completion is delivered via the `storageStart` event:

```cpp
m_logos->storage_module.on("storageStart", [this](const QVariantList& args) {
    // args[0] is a JSON string: {"success":true,"message":""}
    QJsonObject obj = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
    if (!obj["success"].toBool()) {
        QString error = obj["message"].toString();
        // Handle error
    } else {
        // Node is ready
    }
});
```

Similarly, stop the node and listen for the `storageStop` event:

```cpp
m_logos->storage_module.on("storageStop", [this](const QVariantList& args) {
    QJsonObject obj = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
    bool success = obj["success"].toBool();
});
bool ok = m_logos->storage_module.stop();
```

The Logos Storage Module will not stop or clean up the node automatically, and it is the application's responsibility to do so at the appropriate time (e.g. before quitting). Not shutting down the node properly can lead to data loss.

**Important Note**: It is STRONGLY recommended to stop the node before cleaning up the resources. Not doing so can lead to undefined behavior (e.g. node crashing).

To cleanup the resources, call the synchronous `destroy` function:

```cpp
bool ok = m_logos->storage_module.destroy();
```

#### Upload a file

##### Recommended

The straightforward way to upload a file is to use the `uploadUrl` function, passing a local file path:

```cpp
void upload(const std::string& filePath) {
    std::string sessionId = m_logos->storage_module.uploadUrl(filePath);
}
```

You can pass an extra parameter for the chunk size. The default is recommended for most cases:

```cpp
void upload(const std::string& filePath) {
    int chunkSize = 1024 * 64;
    std::string sessionId = m_logos->storage_module.uploadUrl(filePath, chunkSize);
}
```

The method is asynchronous, so again, result tells you that the command was sent to Logos Storage but it doesn't tell you that it was successful. You need to add a listener for `storageUploadDone`:

```cpp
m_logos->storage_module.on("storageUploadDone", [this](const QVariantList& args) {
    // args[0] is a JSON string: {"success":true,"sessionId":"...","cid":"..."}
    QJsonObject obj = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
    bool success = obj["success"].toBool();
    QString sessionId = obj["sessionId"].toString();

    if (!success) {
        QString error = obj["error"].toString();
        // Handle error
    } else {
        m_cid = obj["cid"].toString();
        // Do something super cool with the CID
    }
});
```

The event payload contains `success`, `sessionId`, and either `cid` (on success) or `error` (on failure).

The CID is an identifier of your content. Share it out-of-band to let other people download your content (see [download a file](#download-a-file)).

To track upload progress, subscribe to `storageUploadProgress`:

```cpp
m_logos->storage_module.on("storageUploadProgress", [this](const QVariantList& args) {
    // args[0] is a JSON string: {"success":true,"sessionId":"...","bytes":1024}
    QJsonObject obj = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
    bool success = obj["success"].toBool();
    QString sessionId = obj["sessionId"].toString();

    if (!success) {
        QString error = obj["error"].toString();
        // Handle error
    } else {
        int bytes = obj["bytes"].toInt();
    }
});
```

Be careful! Depending on the size of your data and the chunk size, this function could be called A LOT of times. Progress events are throttled to at most one per percentage point to avoid flooding the caller.

##### Advanced

There is an advanced API that you should use only if you cannot use `uploadUrl`. This API allows you to upload content using a stream. Let's go over the steps.

First you need to create an upload session by providing the filename:

```cpp
    std::string filename = "...";
    std::string sessionId = m_logos->storage_module.uploadInit(filename);

    // Or with custom chunk size
    int chunkSize = 1024 * 64;
    std::string sessionId = m_logos->storage_module.uploadInit(filename, chunkSize);
```

The filename is used to identify the metadata of your content and is useful information in the Manifest. A manifest is an object containing the information about the data identified by a CID. For more information, the dataset spec is available [here](https://lip.logos.co/storage/raw/datasets.html).

The result of `uploadInit` will provide you the `sessionId`. You can use this identifier to upload your chunks one by one:

```cpp
// Read from a file in chunks
std::ifstream file(filePath, std::ios::binary);
std::string chunk(chunkSize, '\0');

while (file.read(chunk.data(), chunkSize) || file.gcount() > 0) {
    chunk.resize(static_cast<size_t>(file.gcount()));
    bool ok = m_logos->storage_module.uploadChunk(sessionId, chunk);
    if (!ok) {
        // Handle error here
    }
    chunk.resize(chunkSize);
}
```

You can subscribe to the same progress event `storageUploadProgress` to get the progress.

While this requires more code, it provides more control. You can easily stop uploading the chunks and resume the upload at any time!

After all the chunks are sent, you need to finalize the upload to get the CID:

```cpp
    std::string cid = m_logos->storage_module.uploadFinalize(sessionId);

Then you can share this CID with others to let them be able to download the file.

#### Download a file

##### Save into a file

The easiest way to download a file is to use the `downloadToUrl` method:

```cpp
    std::string cid = "...";
    std::string filePath = "...";

    std::string sessionId = m_logos->storage_module.downloadToUrl(cid, filePath);

    // Or
    bool local = false;
    std::string sessionId = m_logos->storage_module.downloadToUrl(cid, filePath, local);

    // Or
    int chunkSize = 1024 * 64;
    std::string sessionId = m_logos->storage_module.downloadToUrl(cid, filePath, local, chunkSize);
```

If `local` is set to true, this returns data that is already local to the node; i.e., if your node already has the file, then `downloadToUrl` will read that file and copy it into the specified URL, otherwise it will fail. If `local` is set to false, instead, the node might go through the network to fetch data from other nodes if it is not locally available. If you are unsure, just set this to false.

To get the download progress, subscribe to `storageDownloadProgress`. Note that you will get progress events even for locally available data.

```cpp
m_logos->storage_module.on("storageDownloadProgress", [this](const QVariantList& args) {
    // args[0] is a JSON string: {"success":true,"sessionId":"...","bytes":1024}
    QJsonObject obj = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
    bool success = obj["success"].toBool();
    QString sessionId = obj["sessionId"].toString();

    if (!success) {
        QString error = obj["error"].toString();
        // Handle error
    } else {
        int bytes = obj["bytes"].toInt();
        // Show download progress
    }
});
```

To get the completion event, subscribe to `storageDownloadDone`:

```cpp
m_logos->storage_module.on("storageDownloadDone", [this](const QVariantList& args) {
    // args[0] is a JSON string: {"success":true,"sessionId":"..."}
    QJsonObject obj = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
    bool success = obj["success"].toBool();

    if (!success) {
        QString error = obj["error"].toString();
        // Handle error
    } else {
        // Download complete
    }
});
```

##### Handle the chunks manually

If you do not want to save the data to a file but you want to stream it somewhere you can use `downloadChunks`:

```cpp
std::string cid = "...";

std::string sessionId = m_logos->storage_module.downloadChunks(cid);

// Or
bool local = false;
std::string sessionId = m_logos->storage_module.downloadChunks(cid, local);

// Or
int chunkSize = 1024 * 64;
std::string sessionId = m_logos->storage_module.downloadChunks(cid, local, chunkSize);
```

You can subscribe to the same events: `storageDownloadProgress` and `storageDownloadDone`. But there is an important difference. For `storageDownloadProgress` in chunk mode, the payload contains a `chunk` field instead of `bytes`:

```cpp
m_logos->storage_module.on("storageDownloadProgress", [this](const QVariantList& args) {
    QJsonObject obj = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
    // In chunk mode, "chunk" contains the raw downloaded data
    std::string chunk = obj["chunk"].toString().toStdString();
    // Process the chunk
});
```

**Important note**: The chunk is copied when passed to the callback. If performance is crucial (working with big files) prefer the `downloadToUrl` method.

#### Data management

Several methods are available to manage the data in your node.

- `exists`: Returns `true` if a manifest exists for the CID in your local store.
- `fetch`: Download a file in the background to your store (no event emitted).
- `remove`: Remove a file from your local storage.
- `space`: Returns a `LogosMap` with disk quota information (`totalBlocks`, `quotaMaxBytes`, `quotaUsedBytes`, `quotaReservedBytes`).
- `manifests`: Returns a `LogosList` with all manifests stored locally (each entry has `cid`, `treeCid`, `datasetSize`, `blockSize`, `filename`, `mimetype`).
- `downloadManifest`: Returns a `LogosMap` with the manifest for a given CID.

#### Debug / info

Several methods are available for debugging your Logos Storage:

- `dataDir`: Get the folder that contains the Logos Storage data.
- `debug`: Get a `LogosMap` with debug information (id, addrs, announceAddresses, table).
- `spr`: Get the SPR of your node.
- `peerId`: Get the peer ID of your node.
- `updateLogLevel`: Change the log level of Logos Storage node.

## Module Structure

```
src/
  storage_module_plugin.h   # Plain C++ class (`StorageModuleImpl`) — the public API surface
  storage_module_plugin.cpp # Implementation using libstorage C bindings
tests/
  test_storage.cpp          # Unit tests (mocked libstorage)
  test_storage_module.cpp   # Integration tests (real libstorage)
  mocks/mock_libstorage.cpp # Mock for unit tests
  stubs/libstorage.h        # Stub header for test compilation
metadata.json               # Module config (name, version, interface=universal, deps)
flake.nix                   # Nix build
CMakeLists.txt              # CMake (generated_code/ picked up automatically)
```

The `generated_code/` directory is produced at build time by `logos-cpp-generator` and is not committed to git.

## Tests

```bash
# Run all tests (silent on success)
nix flake check

# Run all tests and always see the output
nix run .#tests

# Run a single test
nix run .#tests -- test_peerId
```

## Requirements

#### Build Tools
- CMake (3.14 or later)
- Ninja build system
- pkg-config

#### Dependencies
- logos-module-builder (build system + code generator)
- logos-liblogos
- nlohmann_json
- [libstorage](https://github.com/logos-storage/logos-storage-nim/tree/chore/improve-c-bindings/library)
