/* WALKER
Explanation:
- Scans a directory tree on several threads and fills a DirTree with the size of every directory.
- This is the Linux version of fsearch's crawler (walk.rs):
    - One getdents64() call returns a few thousand names together with their type, so a
      directory is read in one or two system calls and only regular files need a stat.
    - Files are stat'ed relative to the open directory (fstatat), so the kernel never has to
      walk the whole path again for each file.
    - Every directory is a task. Each thread works depth first on its own tasks and steals the
      oldest tasks of another thread when it runs out, which keeps all threads busy on any tree shape.
- Same rules as the old recursive scan:
    - Symlinks are skipped, so are sockets, pipes and devices.
    - /proc, /sys and network shares (NFS, SMB) are not entered.
    - A directory is only counted once, even if it is reachable twice (bind mounts).
*/

#pragma once

#include "dir_tree.hpp"

#include <atomic>
#include <mutex>
#include <sys/types.h>
#include <unordered_set>
#include <vector>

class Walker {
public:
    // threads == 0 picks a count from the number of CPU cores
    Walker(DirTree& _tree, const std::atomic<bool>& _stop, unsigned threads);

    // Scans the whole tree. Returns when every directory was visited, or once `stop` is set.
    void run();

private:
    struct Task {
        uint32_t dir;      // Node to scan
        dev_t parent_dev;  // Device its parent is on, to notice mount points
    };

    /* Queue
    - Tasks of one thread. The owner pushes and pops at the back (depth first).
    - Other threads steal from the front, where the directories closest to the root are.
    - Aligned so two threads never share a cache line.
    */
    struct alignas(64) Queue {
        std::mutex mutex;
        std::vector<Task> tasks;
    };

    // Per-thread buffers, reused for every directory
    struct Scratch {
        std::vector<char> entries;  // getdents64() output
        std::vector<char> names;    // Sub-directory names of the current directory
        std::vector<Task> stolen;
        std::string path;
    };

    /* Seen
    - Directories already visited, as (device, inode).
    - Split in buckets with their own lock so threads rarely wait for each other.
    */
    struct Inode {
        dev_t dev;
        ino_t ino;

        bool operator==(const Inode& other) const {
            return dev == other.dev && ino == other.ino;
        }
    };

    struct InodeHash {
        std::size_t operator()(const Inode& i) const {
            return std::hash<uint64_t>()(static_cast<uint64_t>(i.ino) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(i.dev));
        }
    };

    struct alignas(64) SeenBucket {
        std::mutex mutex;
        std::unordered_set<Inode, InodeHash> inodes;
    };

    static constexpr size_t SEEN_BUCKETS = 64;

    void work(unsigned self);
    void visit(const Task& task, unsigned self, Scratch& scratch);

    bool pop(unsigned self, Task& task);
    bool steal(unsigned self, Task& task, Scratch& scratch);
    bool firstVisit(dev_t dev, ino_t ino);

    static bool isSkippedFs(int fd);

    DirTree& tree;
    const std::atomic<bool>& stop;

    std::vector<Queue> queues;
    std::vector<SeenBucket> seen;

    // Tasks queued or being worked on. The scan is over when this reaches zero.
    std::atomic<uint64_t> pending{0};
};
