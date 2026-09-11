#include "udb/b_plus_tree.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace udb {
namespace {

constexpr std::uint32_t kMagic = 0x31545042;  // "BPT1" in little-endian.
constexpr std::uint64_t kHeaderMagic = 0x0031485042424455;  // "UDBBPH1\0".
constexpr std::uint32_t kHeaderVersion = 1;
constexpr std::uint8_t kLeaf = 1;
constexpr std::uint8_t kInternal = 2;
constexpr std::uint64_t kInvalidPage = std::numeric_limits<std::uint64_t>::max();

std::uint64_t Read(const Page& page, std::size_t offset, std::size_t width) {
    if (width > 8 || offset > PAGE_SIZE || width > PAGE_SIZE - offset) {
        throw std::runtime_error("B+ tree field is out of page bounds");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(page.data[offset + i])) << (8 * i);
    }
    return value;
}

void Write(Page& page, std::size_t offset, std::size_t width, std::uint64_t value) {
    if (width > 8 || offset > PAGE_SIZE || width > PAGE_SIZE - offset) {
        throw std::runtime_error("B+ tree field is out of page bounds");
    }
    for (std::size_t i = 0; i < width; ++i) {
        page.data[offset + i] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

std::int64_t DecodeKey(std::uint64_t value) {
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(value);
    }
    return -1 - static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() - value);
}

page_id_t DecodePageId(std::uint64_t value) {
    if (value == kInvalidPage) { return -1; }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<page_id_t>::max())) {
        throw std::runtime_error("Invalid B+ tree page ID");
    }
    return static_cast<page_id_t>(value);
}

std::uint64_t EncodePageId(page_id_t value) {
    if (value == -1) { return kInvalidPage; }
    if (value < 0) { throw std::runtime_error("Invalid negative B+ tree page ID"); }
    return static_cast<std::uint64_t>(value);
}

class PinnedPage {
public:
    PinnedPage(BufferPoolManager& pool, page_id_t page_id)
        : pool_(pool), page_id_(page_id), page_(pool.FetchPage(page_id)) {}
    explicit PinnedPage(BufferPoolManager& pool) : pool_(pool) {
        std::tie(page_id_, page_) = pool.NewPage();
    }
    ~PinnedPage() { pool_.UnpinPage(page_id_, dirty_); }
    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;

    page_id_t GetPageId() const { return page_id_; }
    Page& GetPage() { return *page_; }
    void MarkDirty() { dirty_ = true; }

private:
    BufferPoolManager& pool_;
    page_id_t page_id_ = -1;
    Page* page_ = nullptr;
    bool dirty_ = false;
};

}  // namespace

struct BPlusTree::Node {
    bool leaf = true;
    page_id_t parent = -1;
    page_id_t next_leaf = -1;
    std::vector<std::int64_t> keys;
    std::vector<RID> values;
    std::vector<page_id_t> children;
};

BPlusTree::BPlusTree(BufferPoolManager& pool, BPlusTreeOptions options)
    : pool_(pool), options_(options) {
    ValidateOptions();
    root_page_id_ = NewNode(Node{});
}

BPlusTree::BPlusTree(BufferPoolManager& pool, page_id_t root_page_id,
                     BPlusTreeOptions options)
    : pool_(pool), root_page_id_(root_page_id), options_(options) {
    ValidateOptions();
    if (root_page_id < 0) { throw std::invalid_argument("B+ tree root page ID must be nonnegative"); }
    Validate();
}

std::unique_ptr<BPlusTree> BPlusTree::CreateWithHeader(
    BufferPoolManager& pool, BPlusTreeOptions options) {
    auto tree = std::make_unique<BPlusTree>(pool, options);
    tree->header_page_id_ = tree->NewHeaderPage();
    return tree;
}

std::unique_ptr<BPlusTree> BPlusTree::OpenWithHeader(
    BufferPoolManager& pool, page_id_t header_page_id, BPlusTreeOptions options) {
    const auto root_page_id = ReadHeader(pool, header_page_id);
    auto tree = std::make_unique<BPlusTree>(pool, root_page_id, options);
    if (header_page_id == root_page_id) { throw std::runtime_error("B+ tree header aliases its root"); }
    tree->header_page_id_ = header_page_id;
    return tree;
}

void BPlusTree::ValidateOptions() const {
    if (options_.leaf_max_size < 2 || options_.leaf_max_size > BPlusTreeOptions::LEAF_CAPACITY ||
        options_.internal_max_size < 2 ||
        options_.internal_max_size > BPlusTreeOptions::INTERNAL_CAPACITY) {
        throw std::invalid_argument("B+ tree max sizes are outside page capacity");
    }
}

Page BPlusTree::EncodeNode(const Node& node) const {
    if (!std::is_sorted(node.keys.begin(), node.keys.end()) ||
        std::adjacent_find(node.keys.begin(), node.keys.end()) != node.keys.end()) {
        throw std::runtime_error("B+ tree node keys must be strictly increasing");
    }
    Page page;
    Write(page, 0, 4, kMagic);
    Write(page, 4, 1, node.leaf ? kLeaf : kInternal);
    Write(page, 6, 2, node.keys.size());
    Write(page, 8, 8, EncodePageId(node.parent));
    if (node.leaf) {
        if (node.keys.size() > options_.leaf_max_size || node.values.size() != node.keys.size() ||
            !node.children.empty()) {
            throw std::runtime_error("Invalid B+ tree leaf node");
        }
        Write(page, 16, 8, EncodePageId(node.next_leaf));
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            if (node.values[i].page_id < 0) { throw std::runtime_error("Invalid RID in B+ tree leaf"); }
            const auto offset = BPlusTreeOptions::HEADER_SIZE + i * BPlusTreeOptions::LEAF_ENTRY_SIZE;
            Write(page, offset, 8, static_cast<std::uint64_t>(node.keys[i]));
            Write(page, offset + 8, 8, static_cast<std::uint64_t>(node.values[i].page_id));
            Write(page, offset + 16, 2, node.values[i].slot_id);
        }
    } else {
        // keys[i] is the minimum key reachable through children[i + 1].
        if (node.keys.empty() || node.keys.size() > options_.internal_max_size ||
            node.children.size() != node.keys.size() + 1 || !node.values.empty() || node.next_leaf != -1) {
            throw std::runtime_error("Invalid B+ tree internal node");
        }
        Write(page, 16, 8, EncodePageId(node.children.front()));
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const auto offset = BPlusTreeOptions::HEADER_SIZE + i * BPlusTreeOptions::INTERNAL_ENTRY_SIZE;
            Write(page, offset, 8, static_cast<std::uint64_t>(node.keys[i]));
            Write(page, offset + 8, 8, EncodePageId(node.children[i + 1]));
        }
    }
    return page;
}

BPlusTree::Node BPlusTree::DecodeNode(const Page& page, page_id_t page_id) const {
    if (Read(page, 0, 4) != kMagic || Read(page, 5, 1) != 0) {
        throw std::runtime_error("Invalid B+ tree page header");
    }
    const auto type = Read(page, 4, 1);
    const auto count = static_cast<std::size_t>(Read(page, 6, 2));
    Node node;
    node.parent = DecodePageId(Read(page, 8, 8));
    node.leaf = type == kLeaf;
    if (!node.leaf && type != kInternal) { throw std::runtime_error("Unknown B+ tree page type"); }
    if (node.parent == page_id) { throw std::runtime_error("B+ tree page is its own parent"); }
    node.keys.reserve(count);
    if (node.leaf) {
        if (count > options_.leaf_max_size) { throw std::runtime_error("B+ tree leaf exceeds configured capacity"); }
        node.next_leaf = DecodePageId(Read(page, 16, 8));
        if (node.next_leaf == page_id) { throw std::runtime_error("B+ tree leaf points to itself"); }
        node.values.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = BPlusTreeOptions::HEADER_SIZE + i * BPlusTreeOptions::LEAF_ENTRY_SIZE;
            node.keys.push_back(DecodeKey(Read(page, offset, 8)));
            const auto rid_page = DecodePageId(Read(page, offset + 8, 8));
            if (rid_page < 0) { throw std::runtime_error("Invalid RID in B+ tree leaf"); }
            node.values.push_back(RID{rid_page, static_cast<slot_id_t>(Read(page, offset + 16, 2))});
        }
    } else {
        if (count == 0 || count > options_.internal_max_size) {
            throw std::runtime_error("Invalid B+ tree internal key count");
        }
        node.children.reserve(count + 1);
        const auto first_child = DecodePageId(Read(page, 16, 8));
        if (first_child < 0) { throw std::runtime_error("Invalid B+ tree child page ID"); }
        node.children.push_back(first_child);
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = BPlusTreeOptions::HEADER_SIZE + i * BPlusTreeOptions::INTERNAL_ENTRY_SIZE;
            node.keys.push_back(DecodeKey(Read(page, offset, 8)));
            const auto child = DecodePageId(Read(page, offset + 8, 8));
            if (child < 0) { throw std::runtime_error("Invalid B+ tree child page ID"); }
            node.children.push_back(child);
        }
    }
    if (!std::is_sorted(node.keys.begin(), node.keys.end()) ||
        std::adjacent_find(node.keys.begin(), node.keys.end()) != node.keys.end()) {
        throw std::runtime_error("B+ tree page keys are not strictly increasing");
    }
    return node;
}

BPlusTree::Node BPlusTree::ReadNode(page_id_t page_id) const {
    PinnedPage page(pool_, page_id);
    return DecodeNode(page.GetPage(), page_id);
}

void BPlusTree::WriteNode(page_id_t page_id, const Node& node) {
    const auto encoded = EncodeNode(node);
    PinnedPage page(pool_, page_id);
    page.GetPage() = encoded;
    page.MarkDirty();
}

page_id_t BPlusTree::NewNode(const Node& node) {
    const auto encoded = EncodeNode(node);
    PinnedPage page(pool_);
    page.GetPage() = encoded;
    page.MarkDirty();
    return page.GetPageId();
}

page_id_t BPlusTree::NewHeaderPage() {
    PinnedPage page(pool_);
    Page encoded;
    Write(encoded, 0, 8, kHeaderMagic);
    Write(encoded, 8, 4, kHeaderVersion);
    Write(encoded, 16, 8, EncodePageId(root_page_id_));
    page.GetPage() = encoded;
    page.MarkDirty();
    return page.GetPageId();
}

void BPlusTree::WriteHeader() {
    if (header_page_id_ < 0) { return; }
    Page encoded;
    Write(encoded, 0, 8, kHeaderMagic);
    Write(encoded, 8, 4, kHeaderVersion);
    Write(encoded, 16, 8, EncodePageId(root_page_id_));
    PinnedPage page(pool_, header_page_id_);
    page.GetPage() = encoded;
    page.MarkDirty();
}

page_id_t BPlusTree::ReadHeader(BufferPoolManager& pool, page_id_t header_page_id) {
    if (header_page_id < 0) { throw std::invalid_argument("B+ tree header page ID must be nonnegative"); }
    PinnedPage page(pool, header_page_id);
    const auto& bytes = page.GetPage();
    if (Read(bytes, 0, 8) != kHeaderMagic || Read(bytes, 8, 4) != kHeaderVersion ||
        Read(bytes, 12, 4) != 0) {
        throw std::runtime_error("Invalid B+ tree header page");
    }
    const auto root = DecodePageId(Read(bytes, 16, 8));
    if (root < 0) { throw std::runtime_error("Invalid B+ tree root in header"); }
    return root;
}

page_id_t BPlusTree::FindLeaf(std::int64_t key, std::vector<page_id_t>* path) const {
    std::unordered_set<page_id_t> seen;
    auto page_id = root_page_id_;
    while (true) {
        if (!seen.insert(page_id).second) { throw std::runtime_error("B+ tree search encountered a cycle"); }
        const auto node = ReadNode(page_id);
        if (node.leaf) { return page_id; }
        if (path != nullptr) { path->push_back(page_id); }
        const auto child = std::upper_bound(node.keys.begin(), node.keys.end(), key) - node.keys.begin();
        page_id = node.children[static_cast<std::size_t>(child)];
    }
}

std::optional<RID> BPlusTree::GetValue(std::int64_t key) const {
    const auto leaf = ReadNode(FindLeaf(key, nullptr));
    const auto found = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
    if (found == leaf.keys.end() || *found != key) { return std::nullopt; }
    return leaf.values[static_cast<std::size_t>(found - leaf.keys.begin())];
}

bool BPlusTree::Insert(std::int64_t key, RID rid) {
    if (rid.page_id < 0) { throw std::invalid_argument("B+ tree RID page ID must be nonnegative"); }
    std::vector<page_id_t> path;
    const auto leaf_page_id = FindLeaf(key, &path);
    auto leaf = ReadNode(leaf_page_id);
    const auto position = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
    if (position != leaf.keys.end() && *position == key) { return false; }
    const auto index = static_cast<std::size_t>(position - leaf.keys.begin());
    leaf.keys.insert(position, key);
    leaf.values.insert(leaf.values.begin() + static_cast<std::ptrdiff_t>(index), rid);
    if (leaf.keys.size() <= options_.leaf_max_size) {
        WriteNode(leaf_page_id, leaf);
        return true;
    }

    const auto split = leaf.keys.size() / 2;
    Node right;
    right.parent = leaf.parent;
    right.next_leaf = leaf.next_leaf;
    right.keys.assign(leaf.keys.begin() + static_cast<std::ptrdiff_t>(split), leaf.keys.end());
    right.values.assign(leaf.values.begin() + static_cast<std::ptrdiff_t>(split), leaf.values.end());
    leaf.keys.resize(split);
    leaf.values.resize(split);
    const auto right_page_id = NewNode(right);
    leaf.next_leaf = right_page_id;
    WriteNode(leaf_page_id, leaf);
    InsertIntoParent(leaf_page_id, right.keys.front(), right_page_id, std::move(path));
    return true;
}

void BPlusTree::SetParent(page_id_t page_id, page_id_t parent_page_id) {
    auto node = ReadNode(page_id);
    node.parent = parent_page_id;
    WriteNode(page_id, node);
}

void BPlusTree::InsertIntoParent(page_id_t left_page_id, std::int64_t separator,
                                 page_id_t right_page_id, std::vector<page_id_t> path) {
    if (path.empty()) {
        Node root;
        root.leaf = false;
        root.keys = {separator};
        root.children = {left_page_id, right_page_id};
        const auto new_root = NewNode(root);
        SetParent(left_page_id, new_root);
        SetParent(right_page_id, new_root);
        root_page_id_ = new_root;
        WriteHeader();
        return;
    }

    const auto parent_page_id = path.back();
    path.pop_back();
    auto parent = ReadNode(parent_page_id);
    const auto child = std::find(parent.children.begin(), parent.children.end(), left_page_id);
    if (child == parent.children.end()) { throw std::runtime_error("B+ tree parent lost split child"); }
    const auto index = static_cast<std::size_t>(child - parent.children.begin());
    parent.keys.insert(parent.keys.begin() + static_cast<std::ptrdiff_t>(index), separator);
    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(index + 1), right_page_id);
    if (parent.keys.size() <= options_.internal_max_size) {
        WriteNode(parent_page_id, parent);
        SetParent(right_page_id, parent_page_id);
        return;
    }

    const auto middle = parent.keys.size() / 2;
    const auto promoted = parent.keys[middle];
    Node right;
    right.leaf = false;
    right.parent = parent.parent;
    right.keys.assign(parent.keys.begin() + static_cast<std::ptrdiff_t>(middle + 1), parent.keys.end());
    right.children.assign(parent.children.begin() + static_cast<std::ptrdiff_t>(middle + 1),
                          parent.children.end());
    parent.keys.resize(middle);
    parent.children.resize(middle + 1);
    const auto new_right_page_id = NewNode(right);
    WriteNode(parent_page_id, parent);
    for (const auto moved_child : right.children) { SetParent(moved_child, new_right_page_id); }
    InsertIntoParent(parent_page_id, promoted, new_right_page_id, std::move(path));
}

std::size_t BPlusTree::GetHeight() const {
    std::unordered_set<page_id_t> seen;
    std::size_t height = 0;
    auto page_id = root_page_id_;
    while (true) {
        if (!seen.insert(page_id).second) { throw std::runtime_error("B+ tree height encountered a cycle"); }
        ++height;
        const auto node = ReadNode(page_id);
        if (node.leaf) { return height; }
        page_id = node.children.front();
    }
}

void BPlusTree::Validate() const {
    struct Info {
        std::int64_t minimum = 0;
        std::int64_t maximum = 0;
        bool has_key = false;
        std::size_t height = 0;
    };
    std::unordered_set<page_id_t> seen;
    std::vector<std::pair<page_id_t, page_id_t>> leaves;
    std::function<Info(page_id_t, page_id_t)> visit = [&](page_id_t page_id, page_id_t expected_parent) -> Info {
        if (!seen.insert(page_id).second) { throw std::runtime_error("B+ tree contains a cycle or shared child"); }
        const auto node = ReadNode(page_id);
        if (node.parent != expected_parent) { throw std::runtime_error("B+ tree parent page ID mismatch"); }
        if (node.leaf) {
            if (page_id != root_page_id_ && node.keys.empty()) {
                throw std::runtime_error("Non-root B+ tree leaf is empty");
            }
            leaves.emplace_back(page_id, node.next_leaf);
            if (node.keys.empty()) { return Info{0, 0, false, 1}; }
            return Info{node.keys.front(), node.keys.back(), true, 1};
        }
        std::vector<Info> children;
        children.reserve(node.children.size());
        for (const auto child : node.children) { children.push_back(visit(child, page_id)); }
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            if (!children[i].has_key || !children[i + 1].has_key ||
                children[i].maximum >= node.keys[i] || children[i + 1].minimum != node.keys[i]) {
                throw std::runtime_error("B+ tree separator invariant is broken");
            }
        }
        for (std::size_t i = 1; i < children.size(); ++i) {
            if (children[i].height != children[0].height) {
                throw std::runtime_error("B+ tree leaves have different depths");
            }
        }
        return Info{children.front().minimum, children.back().maximum, true, children.front().height + 1};
    };
    visit(root_page_id_, -1);
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const auto expected = i + 1 < leaves.size() ? leaves[i + 1].first : -1;
        if (leaves[i].second != expected) { throw std::runtime_error("B+ tree leaf chain is broken"); }
    }
}

}  // namespace udb
