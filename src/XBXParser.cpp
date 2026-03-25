#include "XBXParser.h"
#include <fstream>
#include <cstring>
#include <regex>
#include <unordered_map>
#include <algorithm>
#include <cstdio>

// ── helpers ──────────────────────────────────────────────

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}

static std::string read_mat_name(const uint8_t* data, size_t ptr) {
    // skip 4-byte hash, read null-padded 28-byte string
    const char* s = reinterpret_cast<const char*>(data + ptr + 4);
    size_t len = strnlen(s, 28);
    return std::string(s, len);
}

static int64_t find_mat_table(const uint8_t* data, size_t size) {
    // look for 'material ', 'smchar', 'blackcat', 'black_cat', 'character'
    const char* patterns[] = {"material ", "smchar", "blackcat", "black_cat", "character"};
    for (const char* pat : patterns) {
        size_t plen = strlen(pat);
        for (size_t i = 0; i + plen < size; ++i) {
            if (memcmp(data + i, pat, plen) == 0)
                return static_cast<int64_t>(i) - 4;
        }
    }
    return -1;
}

// ── Triangle strip → triangle list ───────────────────────
static std::vector<uint32_t> strip_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    tris.reserve(n * 2);
    for (size_t i = 0; i + 2 < n; ++i) {
        uint32_t a = idx[i], b = idx[i+1], c = idx[i+2];
        if (a == b || b == c || a == c) continue;
        a = std::min(a, vc-1); b = std::min(b, vc-1); c = std::min(c, vc-1);
        if (i & 1) { tris.push_back(a); tris.push_back(c); tris.push_back(b); }
        else        { tris.push_back(a); tris.push_back(b); tris.push_back(c); }
    }
    return tris;
}

// ── Main parser ───────────────────────────────────────────
XBXModel* parse_xbx(const std::string& filepath) {
    auto buf = read_file(filepath);
    if (buf.size() < 16) return nullptr;
    const uint8_t* d = buf.data();
    const size_t   sz = buf.size();

    if (memcmp(d, "XBXM", 4) != 0) return nullptr;

    uint32_t chunk_cnt = rd<uint32_t>(d, 0x08);
    uint32_t hdr_sz    = rd<uint32_t>(d, 0x0c);
    uint32_t top       = hdr_sz + chunk_cnt * 0x30;

    if (top + 0x60 > sz) return nullptr;
    uint32_t sm_cnt = rd<uint32_t>(d, top + 0x04);
    if (sm_cnt == 0 || sm_cnt > 64) return nullptr;

    std::vector<uint32_t> sm_ptrs(sm_cnt);
    for (uint32_t i = 0; i < sm_cnt; ++i)
        sm_ptrs[i] = rd<uint32_t>(d, top + 0x40 + i * 8);

    // Build mat -> tex hint
    std::unordered_map<std::string, std::string> mat_to_tex;
    int64_t mt = find_mat_table(d, sz);
    if (mt >= 0) {
        std::unordered_map<uint32_t, std::string> ptrs;
        for (uint32_t off = 0x20; off < std::min((uint32_t)top, 0x300u); off += 4) {
            uint32_t v = rd<uint32_t>(d, off);
            if (v >= (uint32_t)mt && v < (uint32_t)mt + 512 && v + 32 <= sz) {
                ptrs[off] = read_mat_name(d, v);
            }
        }
        std::vector<uint32_t> offs;
        for (auto& kv : ptrs) offs.push_back(kv.first);
        std::sort(offs.begin(), offs.end());

        for (size_t i = 0; i < offs.size(); ++i) {
            auto& mname = ptrs[offs[i]];
            if (mname.rfind("material", 0) != 0) continue;
            for (size_t j = i+1; j < std::min(i+6, offs.size()); ++j) {
                auto& n = ptrs[offs[j]];
                if (n.rfind("material", 0) != 0 &&
                    n != "smcharenv" && n != "smcharenvmorph" && n != "character") {
                    mat_to_tex[mname] = n;
                    break;
                }
            }
        }
    }

    int64_t mat_table_off = mt >= 0 ? mt : (int64_t)sz;

    std::vector<uint32_t> fi_starts(sm_cnt), fi_ends(sm_cnt);
    for (uint32_t i = 0; i < sm_cnt; ++i)
        fi_starts[i] = rd<uint32_t>(d, sm_ptrs[i] + 0x30);
    for (uint32_t i = 0; i < sm_cnt - 1; ++i)
        fi_ends[i] = fi_starts[i+1];
    fi_ends[sm_cnt-1] = (uint32_t)mat_table_off;

    const uint32_t STRIDE = 32;
    auto* model = new XBXModel();
    model->filepath = filepath;

    for (uint32_t si = 0; si < sm_cnt; ++si) {
        uint32_t ptr = sm_ptrs[si];
        if (ptr + 0x60 > sz) continue;

        uint32_t mat_ptr = rd<uint32_t>(d, ptr);
        std::string mat_name = (mat_ptr + 32 < sz) ? read_mat_name(d, mat_ptr) : "sub" + std::to_string(si);

        uint32_t vc       = rd<uint32_t>(d, ptr + 0x40);
        uint32_t vo       = rd<uint32_t>(d, ptr + 0x44);
        uint32_t stride   = rd<uint32_t>(d, ptr + 0x50);
        uint32_t fi_start = fi_starts[si];
        uint32_t fi_end   = fi_ends[si];

        if (vc == 0) continue;
        if (stride != 28 && stride != 32 && stride != 36 && stride != 40 && stride != 48)
            stride = STRIDE;
        if (vo + vc * stride > sz) continue;

        uint32_t n_idx = (fi_end - fi_start) / 2;
        if (n_idx < 3 || fi_start + n_idx * 2 > sz) continue;

        XBXSubmesh sm;
        sm.mat_name = mat_name;
        sm.tex_name = mat_to_tex.count(mat_name) ? mat_to_tex[mat_name] : "";

        // Read vertices: XYZ @ byte 0, UV @ byte 16
        sm.positions.resize(vc);
        sm.uvs.resize(vc);
        for (uint32_t vi = 0; vi < vc; ++vi) {
            size_t base = vo + vi * stride;
            float x = rd<float>(d, base +  0);
            float y = rd<float>(d, base +  4);
            float z = rd<float>(d, base +  8);
            float u = rd<float>(d, base + 16);
            float v = rd<float>(d, base + 20);
            sm.positions[vi] = {x, y, z};
            sm.uvs[vi]       = {u, v};
        }

        // Read triangle strip and convert to list
        const uint16_t* raw = reinterpret_cast<const uint16_t*>(d + fi_start);
        sm.indices = strip_to_list(raw, n_idx, vc);

        if (!sm.indices.empty())
            model->submeshes.push_back(std::move(sm));
    }

    if (model->submeshes.empty()) { delete model; return nullptr; }
    return model;
}
