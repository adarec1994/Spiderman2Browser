#include "Vfs.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// xiso (xdvdfs) on-disk format — confirmed against extract-xiso.c (in@fishtank):
//   • Volume header at byte 0x10000 (+ a per-disc-type base offset):
//       [20] "MICROSOFT*XBOX*MEDIA"  [u32 root_sector]  [u32 root_size]
//     Four known base offsets are probed (standard / global / XGD3 / XGD1).
//   • A directory is a block of `size` bytes at  root_sector*2048 + base.
//   • Entries form a sorted binary tree inside that block:
//       +0  u16 left_offset   (in dwords, relative to block start; 0/0xFFFF=nil)
//       +2  u16 right_offset
//       +4  u32 start_sector
//       +8  u32 file_size
//       +12 u8  attributes    (0x10 = directory)
//       +13 u8  filename_length
//       +14 .. filename
//   • File data is at  start_sector*2048 + base.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr uint64_t SECTOR        = 2048;
constexpr uint64_t HEADER_OFFSET = 0x10000;
constexpr int      ATTR_DIR      = 0x10;
const char*        MAGIC         = "MICROSOFT*XBOX*MEDIA";
constexpr int      MAGIC_LEN     = 20;

// Per-disc-type base offsets (extract-xiso: GLOBAL / XGD3 / XGD1 / standard).
const uint64_t BASE_PROBES[] = { 0ull, 0x0FD90000ull, 0x02080000ull, 0x18300000ull };

struct IsoNode {
    uint32_t sector = 0;
    uint32_t size   = 0;
    bool     is_dir = false;
};

struct IsoImage {
    bool                                  ok = false;
    std::string                           root;       // iso file path (virtual prefix)
    std::string                           root_norm;  // normalized, lowercased
    uint64_t                              base = 0;    // disc lseek offset
    mutable std::ifstream                 stream;
    std::unordered_map<std::string, IsoNode> nodes;   // normalized vpath -> node
    std::vector<std::string>              all;         // every vpath (original case)
};

IsoImage g_img;

// Normalize a path for comparison: backslashes->slashes, drop trailing slash,
// lowercase (Windows is case-insensitive; xiso names are ASCII).
std::string norm(const std::string& p) {
    std::string s = p;
    for (char& c : s) { if (c == '\\') c = '/'; c = (char)std::tolower((unsigned char)c); }
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

template <typename T>
T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }

bool read_at(std::ifstream& f, uint64_t off, void* dst, size_t n) {
    f.clear();
    f.seekg((std::streamoff)off, std::ios::beg);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(dst), (std::streamsize)n);
    return (size_t)f.gcount() == n;
}

// Walk one directory block's binary tree, recording entries and recursing into
// subdirectories. `prefix` is the directory's full virtual path.
void walk_dir(IsoImage& img, uint32_t sector, uint32_t size, const std::string& prefix, int depth) {
    if (size == 0 || depth > 64) return;                 // depth guard vs cyclic/corrupt
    uint64_t base = (uint64_t)sector * SECTOR + img.base;
    std::vector<uint8_t> buf(size);
    if (!read_at(img.stream, base, buf.data(), size)) return;

    // Iterative tree walk over byte-offsets within the block.
    std::vector<uint32_t> stack;
    std::vector<uint8_t>  seen(size, 0);                 // visited-offset guard
    stack.push_back(0);
    while (!stack.empty()) {
        uint32_t off = stack.back();
        stack.pop_back();
        if (off + 14 > size || seen[off]) continue;
        seen[off] = 1;

        uint16_t l   = rd<uint16_t>(&buf[off + 0]);
        uint16_t r   = rd<uint16_t>(&buf[off + 2]);
        uint32_t sec = rd<uint32_t>(&buf[off + 4]);
        uint32_t fsz = rd<uint32_t>(&buf[off + 8]);
        uint8_t  attr= buf[off + 12];
        uint8_t  nlen= buf[off + 13];

        // 0xFFFF padding / nil child; also bounds-check the name.
        if (l != 0 && l != 0xFFFF) stack.push_back((uint32_t)l * 4);
        if (r != 0 && r != 0xFFFF) stack.push_back((uint32_t)r * 4);
        if (nlen == 0 || off + 14 + nlen > size) continue;

        std::string name((const char*)&buf[off + 14], nlen);
        if (name == "." || name == "..") continue;
        std::string vpath = prefix + "/" + name;

        IsoNode node; node.sector = sec; node.size = fsz;
        node.is_dir = (attr & ATTR_DIR) != 0;
        img.nodes[norm(vpath)] = node;
        img.all.push_back(vpath);

        if (node.is_dir) walk_dir(img, sec, fsz, vpath, depth + 1);
    }
}

const IsoNode* find_node(const std::string& path) {
    if (!g_img.ok) return nullptr;
    auto it = g_img.nodes.find(norm(path));
    return it == g_img.nodes.end() ? nullptr : &it->second;
}

} // namespace

namespace vfs {

bool mount_iso(const std::string& iso_path) {
    unmount();
    IsoImage img;
    img.root = iso_path;
    img.root_norm = norm(iso_path);
    img.stream.open(iso_path, std::ios::binary);
    if (!img.stream) return false;

    // Find the volume header by probing the known base offsets.
    char magic[MAGIC_LEN];
    bool found = false;
    for (uint64_t probe : BASE_PROBES) {
        if (read_at(img.stream, HEADER_OFFSET + probe, magic, MAGIC_LEN) &&
            std::memcmp(magic, MAGIC, MAGIC_LEN) == 0) {
            img.base = probe;
            found = true;
            break;
        }
    }
    if (!found) return false;

    // root_sector + root_size follow the 20-byte magic.
    uint32_t root_sector = 0, root_size = 0;
    uint64_t after = HEADER_OFFSET + img.base + MAGIC_LEN;
    if (!read_at(img.stream, after, &root_sector, 4)) return false;
    if (!read_at(img.stream, after + 4, &root_size, 4)) return false;
    if (root_sector == 0 || root_size == 0) return false;

    walk_dir(img, root_sector, root_size, img.root, 0);
    if (img.all.empty()) return false;

    img.ok = true;
    std::sort(img.all.begin(), img.all.end());
    g_img = std::move(img);
    return true;
}

void unmount() {
    if (g_img.stream.is_open()) g_img.stream.close();
    g_img = IsoImage{};
}

bool mounted() { return g_img.ok; }

const std::string& iso_root() { return g_img.root; }

bool is_virtual(const std::string& path) {
    if (!g_img.ok) return false;
    std::string n = norm(path);
    if (n == g_img.root_norm) return true;
    return n.size() > g_img.root_norm.size() &&
           n.compare(0, g_img.root_norm.size(), g_img.root_norm) == 0 &&
           n[g_img.root_norm.size()] == '/';
}

bool exists(const std::string& path) {
    if (is_virtual(path)) {
        if (norm(path) == g_img.root_norm) return true;   // the iso root itself
        return find_node(path) != nullptr;
    }
    std::error_code ec;
    return fs::exists(path, ec);
}

bool is_directory(const std::string& path) {
    if (is_virtual(path)) {
        if (norm(path) == g_img.root_norm) return true;   // root acts as a dir
        const IsoNode* n = find_node(path);
        return n && n->is_dir;
    }
    std::error_code ec;
    return fs::is_directory(path, ec);
}

std::vector<uint8_t> read_file(const std::string& path) {
    if (is_virtual(path)) {
        const IsoNode* n = find_node(path);
        if (!n || n->is_dir) return {};
        std::vector<uint8_t> buf(n->size);
        if (n->size && !read_at(g_img.stream,
                                (uint64_t)n->sector * SECTOR + g_img.base,
                                buf.data(), n->size))
            return {};
        return buf;
    }
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff sz = f.tellg();
    if (sz <= 0) return {};
    std::vector<uint8_t> buf((size_t)sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

std::vector<std::string> walk_files(const std::string& root) {
    std::vector<std::string> out;
    if (is_virtual(root)) {
        std::string base = norm(root);
        for (const std::string& vp : g_img.all) {
            const IsoNode* n = find_node(vp);
            if (!n || n->is_dir) continue;
            std::string nv = norm(vp);
            if (nv.size() > base.size() &&
                nv.compare(0, base.size(), base) == 0 &&
                nv[base.size()] == '/')
                out.push_back(vp);
        }
        return out;
    }
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (e.is_regular_file()) out.push_back(e.path().string());
    }
    return out;
}

std::vector<Entry> list_dir(const std::string& dir) {
    std::vector<Entry> out;
    if (is_virtual(dir)) {
        std::string base = norm(dir);
        for (const std::string& vp : g_img.all) {
            std::string nv = norm(vp);
            if (nv.size() <= base.size() ||
                nv.compare(0, base.size(), base) != 0 ||
                nv[base.size()] != '/')
                continue;
            if (nv.find('/', base.size() + 1) != std::string::npos) continue; // immediate only
            const IsoNode* n = find_node(vp);
            out.push_back(Entry{ vp, n && n->is_dir });
        }
        return out;
    }
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        out.push_back(Entry{ e.path().string(), e.is_directory() });
    }
    return out;
}

} // namespace vfs
