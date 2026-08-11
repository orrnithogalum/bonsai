#include "../../include/core/scanner.hpp"

#include <filesystem>
#include <linux/magic.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <fstream>

bool Scanner::isVirtualFs(const fs::path& path) {
    std::string strPath = path.string();

    if(
        strPath.find("/proc") == 0 ||
        strPath.find("/sys")  == 0
        // strPath.find("/dev")  == 0
        // strPath.find("/run")  == 0
        // strPath.find("/tmp")  == 0
    ) { return true; }

    // Virtual filesystems
    struct statfs stfs;
    if (statfs(path.c_str(), &stfs) != 0)
        return true;

    // Ignore network shares
    switch (stfs.f_type) {
        case NFS_SUPER_MAGIC:
        case SMB_SUPER_MAGIC:
            return true;
        default:
            break;
    }

    return false;
}

void Scanner::loadSnapshot() {
    std::ifstream in(this->db_path);

    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        auto tabPos = line.rfind('\t');
        if (tabPos == std::string::npos) continue;

        std::string path = line.substr(0, tabPos);
        std::string sizeStr = line.substr(tabPos + 1);
        try {
            uint64_t size = std::stoull(sizeStr);
            dir_sizes[path].second = size;
        } catch (const std::exception&) {
            continue;
        }
    }
}

uint64_t Scanner::computeDirSizes(const fs::path& dir) {
    {
        std::lock_guard<std::mutex> lock(stop_mutex);
        if(this->done) { return 0; }
    }

    uint64_t total_size = 0;

    // Some of these entry types can cause loops in scanning and have no real value
    if (!fs::exists(dir) || !fs::is_directory(dir) || isVirtualFs(dir))
        return 0;

    struct stat st;
    if (lstat(dir.c_str(), &st) != 0)
        return 0;

    // Make sure wh haven't visited anything
    Inode inode{st.st_dev, st.st_ino};
    if (visited.find(inode) != visited.end())
        return 0;

    visited.insert(inode);

    for (auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
        try {
            if (fs::is_symlink(entry.path()))
                continue;

            if (fs::is_regular_file(entry.path())) {
                total_size += fs::file_size(entry);
            }
            else if (fs::is_directory(entry.path())) {
                total_size += computeDirSizes(entry.path());
            }

            {
                std::unique_lock lock(map_mutex);
                dir_sizes[dir.string()].first = total_size;
                dir_sizes[dir.string()].second = 0;
            }
        } catch (const fs::filesystem_error&) {}
    }

    return total_size;
}

void Scanner::snapshot() {
    std::lock_guard<std::mutex> lock(map_mutex);

    std::error_code ec;
    if (this->db_path.has_parent_path()) {
        fs::create_directories(this->db_path.parent_path(), ec);
    }

    std::ofstream out(this->db_path, std::ios::trunc);
    if (!out.is_open()) return;

    for (const auto& [path, size] : dir_sizes) {
        out << path << '\t' << size.first << '\n';
    }
}

uint64_t Scanner::get(const fs::path& path) {
    std::lock_guard<std::mutex> lock(map_mutex);
    auto it = dir_sizes.find(path.string());

    if (it != dir_sizes.end())
        return it->second.first;

    return 0;
}

uint64_t Scanner::getSnapped(const fs::path& path) {
    std::lock_guard<std::mutex> lock(map_mutex);
    auto it = dir_sizes.find(path.string());

    if (it != dir_sizes.end())
        return it->second.second;

    return 0;
}

Scanner::ScannerRemoveResult Scanner::remove(const fs::path& path) {
    {
        std::lock_guard<std::mutex> lock(stop_mutex);

        if (!this->done) {
            return ScannerRemoveResult{"Scanner hasn't completed yet.", true};
        }
    }

    std::string target = path.string();

    uint64_t removed_size = 0;

    if(fs::is_directory(path)) {
        {
            std::lock_guard<std::mutex> lock(map_mutex);
            removed_size = dir_sizes[path.string()].first;
        }
    } else {
        std::error_code ec;
        removed_size = fs::file_size(path, ec);
        if (ec) removed_size = 0;
    }

    std::error_code ec;
    try {
        if (fs::is_directory(path)) {
            fs::remove_all(path, ec);
        } else {
            fs::remove(path, ec);
        }
    } catch (const fs::filesystem_error&) {}

    if (ec) {
        return ScannerRemoveResult{ec.message(), true};;
    }

    {
        std::lock_guard<std::mutex> lock(map_mutex);
        fs::path current = path.parent_path();

        while (!current.empty()) {
            dir_sizes[current.string()].first -= removed_size;

            if (dir_sizes[current.string()].first < 0) {
                dir_sizes[current.string()].first = 0;
            }

            if (fs::equivalent(this->path, current)) {
                break;
            }

            current = current.parent_path();
        }

        dir_sizes.erase(target);
    }

    this->snapshot();
    return ScannerRemoveResult{"", false};;
}

void Scanner::scan() {
    computeDirSizes(this->path);
    loadSnapshot();

    {
        std::lock_guard lock(stop_mutex);
        this->done = true;
    }
}

void Scanner::stop() {
    {
        std::lock_guard lock(stop_mutex);
        this->done = true;
    }
}

bool Scanner::isDone() {
    {
        std::lock_guard lock(stop_mutex);
        return this->done;
    }
}
