#include "XBXParser.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <glm/gtc/matrix_transform.hpp>

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
    const char* s = reinterpret_cast<const char*>(data + ptr + 4);
    return std::string(s, strnlen(s, 28));
}

static int64_t find_mat_table(const uint8_t* data, size_t size) {
    const char* pats[] = {"material ", "smchar", "blackcat", "black_cat", "character"};
    for (auto p : pats) {
        size_t plen = strlen(p);
        for (size_t i = 0; i + plen < size; ++i)
            if (memcmp(data+i, p, plen) == 0) return (int64_t)i - 4;
    }
    return -1;
}

static std::vector<uint32_t> strip_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    for (size_t i = 0; i+2 < n; ++i) {
        uint32_t a=idx[i], b=idx[i+1], c=idx[i+2];
        if (a==b || b==c || a==c) continue;
        a=std::min(a,vc-1); b=std::min(b,vc-1); c=std::min(c,vc-1);
        if (i&1) { tris.push_back(a); tris.push_back(c); tris.push_back(b); }
        else     { tris.push_back(a); tris.push_back(b); tris.push_back(c); }
    }
    return tris;
}

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
        sm_ptrs[i] = rd<uint32_t>(d, top + 0x40 + i*8);

    // mat -> tex hint
    std::unordered_map<std::string,std::string> mat_to_tex;
    int64_t mt = find_mat_table(d, sz);
    if (mt >= 0) {
        std::map<uint32_t,std::string> ptrs;
        for (uint32_t off = 0x20; off < std::min((uint32_t)top, 0x300u); off += 4) {
            uint32_t v = rd<uint32_t>(d, off);
            if (v >= (uint32_t)mt && v < (uint32_t)mt + 512 && v+32 <= sz)
                ptrs[off] = read_mat_name(d, v);
        }
        std::vector<uint32_t> offs;
        for (auto& kv : ptrs) offs.push_back(kv.first);
        for (size_t i = 0; i < offs.size(); ++i) {
            if (ptrs[offs[i]].rfind("material",0)!=0) continue;
            for (size_t j = i+1; j < std::min(i+6,offs.size()); ++j) {
                auto& n = ptrs[offs[j]];
                if (n.rfind("material",0)!=0 && n!="smcharenv" && n!="smcharenvmorph" && n!="character")
                { mat_to_tex[ptrs[offs[i]]] = n; break; }
            }
        }
    }

    int64_t mat_table_off = mt >= 0 ? mt : (int64_t)sz;

    std::vector<uint32_t> fi_starts(sm_cnt), fi_ends(sm_cnt);
    for (uint32_t i = 0; i < sm_cnt; ++i)
        fi_starts[i] = rd<uint32_t>(d, sm_ptrs[i] + 0x30);
    for (uint32_t i = 0; i < sm_cnt-1; ++i)
        fi_ends[i] = fi_starts[i+1];
    fi_ends[sm_cnt-1] = (uint32_t)mat_table_off;

    auto* model = new XBXModel();
    model->filepath = filepath;

    // ── Bind pose matrices at 0x2f0 (60 × 4×4 float32, row-major) ──
    constexpr size_t MAT_BASE = 0x2f0;
    constexpr int    N_BONES  = 60;
    model->bind_pose.resize(N_BONES, glm::mat4(1.f));
    for (int i = 0; i < N_BONES; ++i) {
        size_t base = MAT_BASE + i*64;
        if (base + 64 > sz) break;
        glm::mat4 m;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m[c][r] = rd<float>(d, base + (r*4+c)*4); // transpose row→col major
        model->bind_pose[i] = m;
    }

    const uint32_t STRIDE = 32;
    for (uint32_t si = 0; si < sm_cnt; ++si) {
        uint32_t ptr = sm_ptrs[si];
        if (ptr + 0x60 > sz) continue;

        uint32_t mat_ptr = rd<uint32_t>(d, ptr);
        std::string mat_name = (mat_ptr+32 < sz) ? read_mat_name(d, mat_ptr) : "sub"+std::to_string(si);

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
        if (n_idx < 3 || fi_start + n_idx*2 > sz) continue;

        // ── Bone palette: count at ptr+0x08, offset at ptr+0x0c ──
        uint32_t pal_cnt = rd<uint32_t>(d, ptr + 0x08);
        uint32_t pal_off = rd<uint32_t>(d, ptr + 0x0c);
        std::vector<int> palette(pal_cnt, -1);
        if (pal_cnt > 0 && pal_cnt <= 64 && pal_off + pal_cnt*2 <= sz) {
            for (uint32_t pi = 0; pi < pal_cnt; ++pi)
                palette[pi] = (int)rd<uint16_t>(d, pal_off + pi*2);
        }

        XBXSubmesh sm;
        sm.mat_name = mat_name;
        sm.tex_name = mat_to_tex.count(mat_name) ? mat_to_tex[mat_name] : "";

        sm.positions.resize(vc);
        sm.uvs.resize(vc);
        sm.bone_indices.resize(vc, glm::ivec4(-1));
        sm.bone_weights.resize(vc, glm::vec4(0.f));

        for (uint32_t vi = 0; vi < vc; ++vi) {
            size_t base = vo + vi * stride;
            sm.positions[vi] = { rd<float>(d,base+0), rd<float>(d,base+4), rd<float>(d,base+8) };
            sm.uvs[vi]       = { rd<float>(d,base+16), rd<float>(d,base+20) };

            // Bone indices (local palette) at byte 24
            // Bone weights (uint8 /255) at byte 28
            float wsum = 0.f;
            for (int k = 0; k < 4; ++k) {
                uint8_t local_idx = d[base + 24 + k];
                uint8_t wt_byte   = d[base + 28 + k];
                float   wt        = wt_byte / 255.f;

                if (local_idx == 255 || wt == 0.f) {
                    sm.bone_indices[vi][k] = 0; // harmless
                    sm.bone_weights[vi][k] = 0.f;
                } else {
                    int global = (local_idx < (int)palette.size()) ? palette[local_idx] : -1;
                    sm.bone_indices[vi][k] = (global >= 0 && global < N_BONES) ? global : 0;
                    sm.bone_weights[vi][k] = wt;
                    wsum += wt;
                }
            }
            // Normalise weights
            if (wsum > 0.001f)
                sm.bone_weights[vi] /= wsum;
            else
                sm.bone_weights[vi] = glm::vec4(1,0,0,0); // bind to bone 0
        }

        const uint16_t* raw = reinterpret_cast<const uint16_t*>(d + fi_start);
        sm.indices = strip_to_list(raw, n_idx, vc);
        if (!sm.indices.empty())
            model->submeshes.push_back(std::move(sm));
    }

    if (model->submeshes.empty()) { delete model; return nullptr; }
    return model;
}