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
    uint64_t offset = 0;   // absolute byte offset within the mounted image
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
//
// XISO directory layout: entries form a sorted binary tree. Each entry is
//   +0 u16 left  +2 u16 right  (offsets in DWORDS from the dir start; 0=nil)
//   +4 u32 sector +8 u32 size  +12 u8 attr +13 u8 namelen +14 name
// A directory may span multiple 2048-byte sectors; entries never cross a sector
// boundary, so the gap before a boundary is filled with 0xFFFF padding. When a
// child offset lands on that padding, the real entry is at the start of the NEXT
// sector — skip forward to it rather than treating 0xFFFF as a nil child.
void walk_dir(IsoImage& img, uint32_t sector, uint32_t size, const std::string& prefix, int depth) {
    if (size == 0 || depth > 64) return;                 // depth guard vs cyclic/corrupt
    uint64_t base = (uint64_t)sector * SECTOR + img.base;
    std::vector<uint8_t> buf(size);
    if (!read_at(img.stream, base, buf.data(), size)) return;

    // Iterative tree walk. Stack holds byte offsets within the block.
    std::vector<uint32_t> stack;
    std::vector<uint8_t>  seen(size, 0);                 // visited-offset guard
    stack.push_back(0);
    while (!stack.empty()) {
        uint32_t off = stack.back();
        stack.pop_back();

        // Padding skip: if this offset sits on a 0xFFFF marker, advance to the
        // next sector boundary where the real entry begins (multi-sector dirs).
        while (off + 2 <= size && rd<uint16_t>(&buf[off]) == 0xFFFF) {
            uint32_t next = (off & ~(uint32_t)(SECTOR - 1)) + SECTOR;
            if (next <= off) break;                      // overflow guard
            off = next;
        }
        if (off + 14 > size || seen[off]) continue;
        seen[off] = 1;

        uint16_t l   = rd<uint16_t>(&buf[off + 0]);
        uint16_t r   = rd<uint16_t>(&buf[off + 2]);
        uint32_t sec = rd<uint32_t>(&buf[off + 4]);
        uint32_t fsz = rd<uint32_t>(&buf[off + 8]);
        uint8_t  attr= buf[off + 12];
        uint8_t  nlen= buf[off + 13];

        // Children are DWORD offsets; 0 = nil (0xFFFF padding handled on next pop).
        if (l != 0) stack.push_back((uint32_t)l * 4);
        if (r != 0) stack.push_back((uint32_t)r * 4);
        if (nlen == 0 || off + 14 + nlen > size) continue;

        std::string name((const char*)&buf[off + 14], nlen);
        if (name == "." || name == "..") continue;
        std::string vpath = prefix + "/" + name;

        IsoNode node; node.offset = (uint64_t)sec * SECTOR + img.base; node.size = fsz;
        node.is_dir = (attr & ATTR_DIR) != 0;
        img.nodes[norm(vpath)] = node;
        img.all.push_back(vpath);

        if (node.is_dir) walk_dir(img, sec, fsz, vpath, depth + 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// amalga_xb.pak ("SFS") container — ported from spiderman2.bms (QuickBMS).
// A 2-level archive: the .pak holds N "file" entries, each itself a sub-archive
// holding the real assets (.xbx/.dat/.dds...). Data is stored UNCOMPRESSED, so
// every asset is a contiguous byte range we map 1:1 into the VFS. Offsets in the
// tables are relative to the .pak start; `pak` is the .pak's absolute offset in
// the image, so absolute = pak + relative.
//
// .pak header (after a 16-byte preamble):
//   +0x10 base_off  +0x14 info_off  +0x18 info_size   (file table = info_size/0x38)
//   +0x24 folders_off  +0x28 folders_size             (folder table = /0x20)
// file entry (0x38):  name[0x20], u32 off, u32 size, u32 z, u32 z, u32 folder_id, u32 dummy
// sub-archive (at pak + base_off + entry.off):
//   +0x18 file_off (data base, relative)   +0x4c u16 sub_count
//   sub-file table ~+0x258 (skip leading 0 / high-bit dwords to the first name)
// sub entry (0x28):   name[0x20], u32 off (rel. file_off), u32 size
// ─────────────────────────────────────────────────────────────────────────────

uint32_t pak_u32(std::ifstream& f, uint64_t off) { uint32_t v = 0; read_at(f, off, &v, 4); return v; }
uint16_t pak_u16(std::ifstream& f, uint64_t off) { uint16_t v = 0; read_at(f, off, &v, 2); return v; }
std::string pak_str(std::ifstream& f, uint64_t off, size_t maxlen) {
    std::vector<char> b(maxlen + 1, 0);
    read_at(f, off, b.data(), maxlen);
    return std::string(b.data(), strnlen(b.data(), maxlen));
}

// Register every ancestor directory of `vpath` (below the root) as a dir node so
// is_directory()/exists() work for the synthetic pak folder tree.
void ensure_dirs(IsoImage& img, const std::string& vpath) {
    std::string nv = norm(vpath);
    size_t slash = nv.find('/', img.root_norm.size() + 1);
    while (slash != std::string::npos) {
        std::string dir = nv.substr(0, slash);
        if (img.nodes.find(dir) == img.nodes.end()) {
            IsoNode d; d.is_dir = true;
            img.nodes[dir] = d;
        }
        slash = nv.find('/', slash + 1);
    }
}

// Parse one amalga .pak and register all its sub-files as virtual nodes under
// `packs_dir` (the .pak's parent vpath, e.g. "<iso>/data/packs").
void parse_pak(IsoImage& img, uint64_t pak, uint32_t pak_size, const std::string& packs_dir) {
    std::ifstream& f = img.stream;

    uint32_t base_off     = pak_u32(f, pak + 0x10);
    uint32_t info_off     = pak_u32(f, pak + 0x14);
    uint32_t info_size    = pak_u32(f, pak + 0x18);
    uint32_t folders_off  = pak_u32(f, pak + 0x24);
    uint32_t folders_size = pak_u32(f, pak + 0x28);
    if (info_size == 0 || info_size > pak_size) return;

    uint32_t n_files   = info_size / 0x38;
    uint32_t n_folders = folders_size / 0x20;

    std::vector<std::string> paths(n_folders);
    for (uint32_t i = 0; i < n_folders; ++i)
        paths[i] = pak_str(f, pak + folders_off + (uint64_t)i * 0x20, 0x20);

    for (uint32_t fi = 0; fi < n_files; ++fi) {
        uint64_t e = pak + info_off + (uint64_t)fi * 0x38;
        std::string name = pak_str(f, e, 0x20);
        uint32_t f_off = pak_u32(f, e + 0x20);
        uint32_t id    = pak_u32(f, e + 0x30);
        if (name.empty()) continue;

        std::string folder = (id < paths.size()) ? paths[id] : std::string();
        std::string dir = folder.empty() ? name : (folder + "/" + name);

        uint64_t sub       = pak + base_off + f_off;       // sub-archive start (absolute)
        uint32_t file_off  = pak_u32(f, sub + 0x18);
        uint64_t data_base = sub + file_off;               // sub-file data base (absolute)
        uint16_t sub_count = pak_u16(f, sub + 0x4c);
        if (sub_count == 0 || sub_count > 4096) continue;

        // Sub-file table is at sub+0x258, fixed 0x28 stride:
        //   name[0x20], u32 off (rel. data_base), u32 size.
        // The byte at name+0x1f (the name field's last byte, which is 0 unless the
        // name is a full 31 chars) is the asset TYPE → file extension. Names carry
        // NO extension and NO _NNNNNNNN suffix in the pak; quickbms synthesizes
        // both. We reproduce it: one shared counter per base name (by appearance
        // order), and the type byte selects the extension. The exact suffix number
        // need not match quickbms — the viewer resolves models/textures/skeletons
        // by base stem — but it must be unique & stem-compatible, which it is.
        uint64_t pos = sub + 0x258;
        std::unordered_map<std::string, int> seen;   // base name -> next index
        for (uint32_t x = 0; x < sub_count; ++x) {
            uint64_t ep = pos + (uint64_t)x * 0x28;
            std::string sn = pak_str(f, ep, 0x20);
            uint8_t  type   = 0; read_at(f, ep + 0x1f, &type, 1);
            uint32_t s_off  = pak_u32(f, ep + 0x20);
            uint32_t s_size = pak_u32(f, ep + 0x24);
            if (sn.empty() || s_size == 0) continue;

            const char* ext;
            switch (type) {
                case 0x04: ext = ".dds"; break;   // DDS texture
                case 0x11: ext = ".xbx"; break;   // XBXM model
                case 0x1b: ext = ".pfp"; break;   // particle
                case 0x1c: ext = ".pft"; break;   // particle
                default:   ext = ".dat"; break;   // skeleton/anim/data
            }
            int n = seen[sn]++;            // 0 for the first occurrence of this base
            char fname[64];
            if (n == 0) snprintf(fname, sizeof(fname), "%s%s", sn.c_str(), ext);
            else        snprintf(fname, sizeof(fname), "%s_%08d%s", sn.c_str(), n, ext);

            std::string vpath = packs_dir + "/" + dir + "/" + fname;
            ensure_dirs(img, vpath);
            IsoNode node; node.offset = data_base + s_off; node.size = s_size; node.is_dir = false;
            img.nodes[norm(vpath)] = node;
            img.all.push_back(vpath);
        }
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

    // Expand any amalga .pak containers on the disc so their packed assets (the
    // real .xbx/.dat/.dds) appear as virtual files. Snapshot the .pak nodes first
    // since parse_pak appends to img.all/img.nodes while we iterate.
    std::vector<std::pair<std::string, IsoNode>> paks;
    for (const std::string& vp : img.all) {
        std::string nv = norm(vp);
        if (nv.size() > 4 && nv.compare(nv.size() - 4, 4, ".pak") == 0) {
            auto it = img.nodes.find(nv);
            if (it != img.nodes.end() && !it->second.is_dir)
                paks.push_back({ vp, it->second });
        }
    }
    // Expand each amalga_xb.pak so its packed assets become virtual files,
    // exactly as if the .pak had been extracted to a folder. Verified against the
    // quickbms output: sizes + paths match the extracted tree (e.g. ARMORED_THUG
    // 40/45 by exact name, 45/45 by stem+bytes).
    for (auto& pk : paks) {
        std::string parent = pk.first.substr(0, pk.first.find_last_of('/'));
        parse_pak(img, pk.second.offset, pk.second.size, parent);
    }

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
        if (n->size && !read_at(g_img.stream, n->offset, buf.data(), n->size))
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
