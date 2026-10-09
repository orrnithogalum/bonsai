#include "../../include/core/scanner.hpp"

#include "../../include/config/config.hpp"
#include "../../include/core/walker.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

void Scanner::loadSnapshot() {
    std::ifstream in(this->db_path);

    auto config = Config::get();
    if (!in.is_open() || !config.ENABLE_FOLDER_SIZE_PERCENTAGES) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        auto tabPos = line.rfind('\t');
        if (tabPos == std::string::npos) continue;

        std::string path = line.substr(0, tabPos);
        std::string sizeStr = line.substr(tabPos + 1);
        try {
            uint64_t size = std::stoull(sizeStr);

            // Directories that no longer exist are dropped
            uint32_t id = tree.find(path);
            if (id != DirTree::NONE)
                tree.node(id).snapped.store(size, std::memory_order_relaxed);
        } catch (const std::exception&) {
            continue;
        }
    }
}

void Scanner::snapshot() {
    auto config = Config::get();
    if(!config.ENABLE_FOLDER_SIZE_PERCENTAGES) return;

    std::lock_guard<std::mutex> lock(snapshot_mutex);

    std::error_code ec;
    if (this->db_path.has_parent_path()) {
        fs::create_directories(this->db_path.parent_path(), ec);
    }

    std::ofstream out(this->db_path, std::ios::trunc);
    if (!out.is_open()) return;

    tree.forEach([&out](const std::string& path, const DirTree::Node& dir) {
        uint64_t size = dir.size.load(std::memory_order_relaxed);

        // An empty directory reads back as 0 anyway
        if (size != 0) {
            out << path << '\t' << size << '\n';
        }
    });
}

uint64_t Scanner::get(const fs::path& path) {
    uint32_t id = tree.find(path.native());

    if (id != DirTree::NONE)
        return tree.node(id).size.load(std::memory_order_relaxed);

    return 0;
}

uint64_t Scanner::getSnapped(const fs::path& path) {
    auto config = Config::get();
    if(!config.ENABLE_FOLDER_SIZE_PERCENTAGES) return 0;

    uint32_t id = tree.find(path.native());

    if (id != DirTree::NONE)
        return tree.node(id).snapped.load(std::memory_order_relaxed);

    return 0;
}

Scanner::ScannerRemoveResult Scanner::remove(const fs::path& path) {
    if (!this->done) {
        return ScannerRemoveResult{"Scanner hasn't completed yet.", true};
    }

    uint64_t removed_size = 0;
    uint32_t removed_dir = DirTree::NONE;

    if(fs::is_directory(path)) {
        removed_dir = tree.find(path.native());

        if (removed_dir != DirTree::NONE) {
            removed_size = tree.node(removed_dir).size.load();
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

    // Every directory above the removed entry just got smaller
    uint32_t parent = tree.find(path.parent_path().native());

    if (parent != DirTree::NONE) {
        tree.subtract(parent, removed_size);
    }

    if (removed_dir != DirTree::NONE) {
        tree.node(removed_dir).removed = true;
    }

    this->snapshot();
    return ScannerRemoveResult{"", false};;
}

void Scanner::scan() {
    Walker walker(tree, done, std::max(0, Config::get().SCANNER_THREADS));
    walker.run();

    loadSnapshot();
    this->done = true;
}

void Scanner::stop() {
    this->done = true;
}

bool Scanner::isDone() {
    return this->done;
}
