/* SCANNER
Explanation:
- This class scans a filesystem path and computes the total sizes of directories.
- The scan runs on several threads (see Walker) and stores its results in a tree (see DirTree).
- A directory is never counted twice, even if it can be reached through two paths.
- Sizes can be read while the scan is running: they are atomics, so get() never waits for the scan.
- Virtual filesystems (like /proc, /sys) and network shares (NFS, SMB) are ignored.
- Public interface includes:
    - scan() to start scanning the root path.
    - get(path) to retrieve the total size of a directory (thread-safe).
*/

#pragma once

#include "dir_tree.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

class Scanner {
public:
    /* ScannerRemoveResult
    - Purely used for error messages and error codes
    */
    struct ScannerRemoveResult {
        std::string reason;
        bool failed;
    };

    struct SnapshotDiff {
        uint64_t old_size;
        uint64_t new_size;
        double percent_change; // (new-old)/old * 100, 0 if old==0 and new==0
    };

    Scanner(const fs::path& _path, const fs::path& _db_path) : path(_path), db_path(_db_path), tree(_path.string()) {};

    void scan();
    void stop();
    bool isDone();

    void snapshot();
    void loadSnapshot();

    ScannerRemoveResult remove(const fs::path& path);

    uint64_t get(const fs::path& path);
    uint64_t getSnapped(const fs::path& path);

private:
    fs::path path;
    fs::path db_path;

    DirTree tree;

    // Set when the scan has completed, or to make it stop early
    std::atomic<bool> done{false};

    // Only one thread writes the snapshot file at a time
    std::mutex snapshot_mutex;
};
