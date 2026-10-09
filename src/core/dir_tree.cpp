#include "../../include/core/dir_tree.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>

DirTree::DirTree(const std::string& root_path) : root(root_path) {
    while (!root.empty() && root.back() == '/')
        root.pop_back();

    reserve(ROOT, ROOT);
}

DirTree::~DirTree() {
    const uint64_t used = std::min<uint64_t>(next_id.load(), NONE);

    for (uint64_t id = 0; id < used; id++) {
        uint32_t offset;
        Node* chunk = chunks[chunkOf(static_cast<uint32_t>(id), offset)].load();

        if (chunk)
            delete chunk[offset].children.load();
    }

    for (auto& chunk : chunks)
        delete[] chunk.load();
}

int DirTree::chunkOf(uint32_t id, uint32_t& offset) {
    const uint64_t v = (static_cast<uint64_t>(id) >> FIRST_CHUNK_BITS) + 1;
    const int chunk = 63 - __builtin_clzll(v);

    offset = id - static_cast<uint32_t>(((uint64_t{1} << chunk) - 1) << FIRST_CHUNK_BITS);
    return chunk;
}

DirTree::Node& DirTree::node(uint32_t id) const {
    uint32_t offset;
    const int chunk = chunkOf(id, offset);

    return chunks[chunk].load(std::memory_order_acquire)[offset];
}

uint32_t DirTree::count() const {
    return static_cast<uint32_t>(std::min<uint64_t>(next_id.load(), NONE));
}

// Makes sure the chunks holding ids first..last exist.
bool DirTree::reserve(uint64_t first, uint64_t last) {
    if (last >= NONE)
        return false;

    uint32_t unused;
    const int from = chunkOf(static_cast<uint32_t>(first), unused);
    const int to = chunkOf(static_cast<uint32_t>(last), unused);

    for (int c = from; c <= to; c++) {
        if (chunks[c].load(std::memory_order_acquire))
            continue;

        std::lock_guard<std::mutex> lock(chunk_mutex);
        if (!chunks[c].load(std::memory_order_relaxed))
            chunks[c].store(new Node[size_t{1} << (FIRST_CHUNK_BITS + c)], std::memory_order_release);
    }

    return true;
}

const DirTree::Block* DirTree::attach(uint32_t dir, const std::vector<char>& names, uint32_t count) {
    if (count == 0)
        return nullptr;

    // Where each name starts, then the same list sorted by name
    std::vector<uint32_t> starts;
    starts.reserve(count);

    for (uint32_t at = 0; at < names.size(); at += std::strlen(&names[at]) + 1)
        starts.push_back(at);

    std::sort(starts.begin(), starts.end(), [&names](uint32_t a, uint32_t b) {
        return std::strcmp(&names[a], &names[b]) < 0;
    });

    auto* block = new Block();
    block->offsets.reserve(count + 1);
    block->names.reserve(names.size());

    for (uint32_t start : starts) {
        const size_t len = std::strlen(&names[start]) + 1;

        block->offsets.push_back(static_cast<uint32_t>(block->names.size()));
        block->names.insert(block->names.end(), &names[start], &names[start] + len);
    }
    block->offsets.push_back(static_cast<uint32_t>(block->names.size()));

    // Ids for the children: one run, taken in a single step
    const uint64_t first = next_id.fetch_add(count, std::memory_order_relaxed);
    if (!reserve(first, first + count - 1)) {
        delete block;
        return nullptr;
    }

    block->first = static_cast<uint32_t>(first);
    for (uint32_t i = 0; i < count; i++)
        node(block->first + i).parent = dir;

    // From here on other threads can see the children
    node(dir).children.store(block, std::memory_order_release);
    return block;
}

void DirTree::add(uint32_t dir, uint64_t bytes) {
    if (bytes == 0)
        return;

    for (uint32_t id = dir; id != NONE; id = node(id).parent)
        node(id).size.fetch_add(bytes, std::memory_order_relaxed);
}

void DirTree::subtract(uint32_t dir, uint64_t bytes) {
    for (uint32_t id = dir; id != NONE; id = node(id).parent) {
        auto& size = node(id).size;
        uint64_t current = size.load(std::memory_order_relaxed);

        // Never go below zero: the disk may have changed since the scan
        while (!size.compare_exchange_weak(current, current > bytes ? current - bytes : 0)) {}
    }
}

uint32_t DirTree::find(std::string_view path) const {
    if (path.size() < root.size() || path.compare(0, root.size(), root) != 0)
        return NONE;

    path.remove_prefix(root.size());
    if (!path.empty() && path.front() != '/')
        return NONE; // "/home/user2" is not inside "/home/user"

    uint32_t id = ROOT;

    while (true) {
        while (!path.empty() && path.front() == '/')
            path.remove_prefix(1);

        if (node(id).removed.load(std::memory_order_relaxed))
            return NONE;

        if (path.empty())
            return id;

        const size_t slash = path.find('/');
        const std::string_view name = path.substr(0, slash);
        path.remove_prefix(slash == std::string_view::npos ? path.size() : slash);

        const Block* block = node(id).children.load(std::memory_order_acquire);
        if (!block)
            return NONE;

        // Binary search in the sorted names of this directory
        uint32_t low = 0;
        uint32_t high = block->count();

        while (low < high) {
            const uint32_t mid = low + (high - low) / 2;

            if (block->name(mid) < name)
                low = mid + 1;
            else
                high = mid;
        }

        if (low == block->count() || block->name(low) != name)
            return NONE;

        id = block->first + low;
    }
}

void DirTree::path(uint32_t id, std::string& out) const {
    if (id == ROOT) {
        out = root.empty() ? "/" : root;
        return;
    }

    // First pass measures, second pass writes the names from the end backwards
    size_t length = root.size();
    for (uint32_t at = id; at != ROOT; at = node(at).parent) {
        const Block* block = node(node(at).parent).children.load(std::memory_order_acquire);
        length += 1 + block->name(at - block->first).size();
    }

    out.resize(length);
    std::memcpy(out.data(), root.data(), root.size());

    size_t end = length;
    for (uint32_t at = id; at != ROOT; at = node(at).parent) {
        const Block* block = node(node(at).parent).children.load(std::memory_order_acquire);
        const std::string_view name = block->name(at - block->first);

        end -= name.size();
        std::memcpy(out.data() + end, name.data(), name.size());
        out[--end] = '/';
    }
}

void DirTree::forEach(const std::function<void(const std::string&, const Node&)>& fn) const {
    struct Frame {
        uint32_t id;
        size_t path_length; // Length of the parent's path
        std::string_view name;
    };

    const std::string slash = "/";
    std::string current = root;
    std::vector<Frame> stack;
    stack.push_back({ROOT, root.size(), {}});

    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();

        const Node& dir = node(frame.id);
        if (dir.removed.load(std::memory_order_relaxed))
            continue;

        current.resize(frame.path_length);
        if (frame.id != ROOT) {
            current += '/';
            current += frame.name;
        }

        fn(current.empty() ? slash : current, dir);

        const Block* block = dir.children.load(std::memory_order_acquire);
        if (!block)
            continue;

        for (uint32_t i = block->count(); i-- > 0;)
            stack.push_back({block->first + i, current.size(), block->name(i)});
    }
}
