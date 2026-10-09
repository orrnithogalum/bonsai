/* DIR TREE
Explanation:
- Every scanned directory is one node in a tree, instead of one entry in a path -> size hashmap.
- This is the layout idea of fsearch's name index (index.rs): a directory's sub-directories live
  together in one sorted block, so a path is resolved by a binary search per component and no
  full path string is ever stored.
- Built for one scan running on many threads while the UI reads from it:
    - Nodes live in chunks that never move, so a node reference stays valid forever.
    - A directory's block of children is published once, fully built, through an atomic pointer.
    - Sizes are atomics: scanner threads add to them, UI threads read them, nobody takes a lock.
- A node's size is the total of its whole subtree. It grows while the scan is running.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class DirTree {
public:
    static constexpr uint32_t NONE = UINT32_MAX;
    static constexpr uint32_t ROOT = 0;

    /* Block
    - The sub-directories of one directory, sorted by name.
    - Child number i is node `first + i`: ids are handed out in runs, so no id is stored per child.
    - Names are stored back to back, each followed by a '\0'.
    - Never modified once it is attached to its directory.
    */
    struct Block {
        uint32_t first = 0;
        std::vector<uint32_t> offsets; // count + 1 entries into `names`
        std::vector<char> names;

        uint32_t count() const { return static_cast<uint32_t>(offsets.size()) - 1; }
        const char* c_str(uint32_t i) const { return names.data() + offsets[i]; }
        std::string_view name(uint32_t i) const { return {c_str(i), offsets[i + 1] - offsets[i] - 1}; }
    };

    struct Node {
        std::atomic<uint64_t> size{0};            // Bytes in this directory and everything below it
        std::atomic<uint64_t> snapped{0};         // Same, as saved by the previous run
        std::atomic<const Block*> children{nullptr}; // nullptr: not listed yet, or no sub-directories
        uint32_t parent = NONE;
        std::atomic<bool> removed{false};
    };

    explicit DirTree(const std::string& root_path);
    ~DirTree();

    DirTree(const DirTree&) = delete;
    DirTree& operator=(const DirTree&) = delete;

    Node& node(uint32_t id) const;

    /* attach(dir, names)
    - Records the sub-directories of `dir`. `names` are '\0' separated and need no order.
    - Returns the published block (nullptr if the tree is full), whose children can now be scanned.
    - Called at most once per directory, by the thread that listed it.
    */
    const Block* attach(uint32_t dir, const std::vector<char>& names, uint32_t count);

    // Adds / removes `bytes` on `dir` and on every directory above it.
    void add(uint32_t dir, uint64_t bytes);
    void subtract(uint32_t dir, uint64_t bytes);

    // Resolves an absolute path to its node, or NONE if it isn't a (known) directory under the root.
    uint32_t find(std::string_view path) const;

    // Writes the absolute path of `id` into `out`.
    void path(uint32_t id, std::string& out) const;

    // Calls fn(path, node) for every directory, parents before children.
    void forEach(const std::function<void(const std::string&, const Node&)>& fn) const;

    uint32_t count() const;

private:
    /* Node storage
    - Chunk c holds (1024 << c) nodes, so 23 chunks cover every 32 bit id.
    - A chunk is allocated once and never moves or shrinks.
    */
    static constexpr int FIRST_CHUNK_BITS = 10;
    static constexpr int MAX_CHUNKS = 23;

    static int chunkOf(uint32_t id, uint32_t& offset);
    bool reserve(uint64_t first, uint64_t last);

    std::atomic<Node*> chunks[MAX_CHUNKS] = {};
    std::atomic<uint64_t> next_id{1};
    std::mutex chunk_mutex;

    std::string root; // Without trailing slash, so "/" is stored as ""
};
