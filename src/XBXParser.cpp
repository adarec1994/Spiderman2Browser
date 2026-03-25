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


// Triangle strip: each position (degenerate or not) flips parity.
// Tracks actual parity not raw index, so degenerates used as restarts work correctly.
static std::vector<uint32_t> strip_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    int parity = 0;
    for (size_t i = 0; i + 2 < n; ++i) {
        uint32_t a=idx[i], b=idx[i+1], c=idx[i+2];
        if (a >= vc || b >= vc || c >= vc) { parity ^= 1; continue; }
        if (a==b || b==c || a==c)          { parity ^= 1; continue; }
        if (parity) { tris.push_back(a); tris.push_back(c); tris.push_back(b); }
        else        { tris.push_back(a); tris.push_back(b); tris.push_back(c); }
        parity ^= 1;
    }
    return tris;
}

// Triangle list: every 3 indices = one triangle, no strip winding logic.
static std::vector<uint32_t> trilist_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    for (size_t i = 0; i + 2 < n; i += 3) {
        uint32_t a=idx[i], b=idx[i+1], c=idx[i+2];
        if (a >= vc || b >= vc || c >= vc) continue;
        if (a==b || b==c || a==c) continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
    }
    return tris;
}

// Quad list: every 4 indices = one quad split into 2 triangles.
static std::vector<uint32_t> quadlist_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    for (size_t i = 0; i + 3 < n; i += 4) {
        uint32_t a=idx[i], b=idx[i+1], c=idx[i+2], dd=idx[i+3];
        if (a>=vc||b>=vc||c>=vc||dd>=vc) continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
        tris.push_back(a); tris.push_back(c); tris.push_back(dd);
    }
    return tris;
}

XBXModel* parse_xbx(const std::string& filepath) {
    auto buf = read_file(filepath);
    if (buf.size() < 16) return nullptr;
    const uint8_t* d = buf.data();
    const size_t   sz = buf.size();

    if (memcmp(d, "XBXM", 4) != 0) return nullptr;

    uint32_t hdr_sz    = rd<uint32_t>(d, 0x0c);

    // Locate geom_base: scan ALL chunks for the dword with tag 0x02xxxxxx,
    // geom_base is the immediately following dword + 4.
    // (2-chunk items have the sentinel in chunk[0]; 3-5 chunk characters may have it in later chunks)
    uint32_t chunk_cnt = rd<uint32_t>(d, 0x08);
    uint32_t geom_base = 0;
    for (uint32_t ci = 0; ci < chunk_cnt && !geom_base; ++ci) {
        uint32_t base = hdr_sz + ci * 0x30;
        if (base + 12*4 > sz) break;
        for (uint32_t i = 0; i < 12; ++i) {
            uint32_t v = rd<uint32_t>(d, base + i*4);
            if ((v & 0xFF000000) == 0x02000000 && i + 1 < 12) {
                geom_base = rd<uint32_t>(d, base + (i+1)*4) + 4;
                break;
            }
        }
    }
    if (geom_base == 0 || geom_base + 0x48 > sz) return nullptr;

    uint32_t sm_cnt = rd<uint32_t>(d, geom_base + 0x04);
    if (sm_cnt == 0 || sm_cnt > 64) return nullptr;

    std::vector<uint32_t> sm_ptrs(sm_cnt);
    for (uint32_t i = 0; i < sm_cnt; ++i)
        sm_ptrs[i] = rd<uint32_t>(d, geom_base + 0x40 + i*8);

    // Texture names are read directly from the mat struct per-submesh below.
    // (mat_ptr+0x24 = shader type; mat_ptr+0x44 = real tex when +0x24 is generic)

    // fi_start at ptr+0x30; if zero, fall back to ptr+0x38.
    // When using the p38 fallback, first 6 uint16s are a sub-header — skip them and treat as strip.
    // fi_end = fi_start + idx_cnt*2 where idx_cnt is at ptr+0x2c.
    constexpr uint32_t P38_HDR_SKIP = 6; // uint16s
    std::vector<uint32_t> fi_starts(sm_cnt), fi_ends(sm_cnt);
    std::vector<bool>     fi_from_p38(sm_cnt, false);
    for (uint32_t i = 0; i < sm_cnt; ++i) {
        uint32_t p30     = rd<uint32_t>(d, sm_ptrs[i] + 0x30);
        uint32_t idx_cnt = rd<uint32_t>(d, sm_ptrs[i] + 0x2c);
        uint32_t fi_start;
        if (p30 != 0) {
            fi_start = p30;
        } else {
            fi_from_p38[i] = true;
            fi_start  = rd<uint32_t>(d, sm_ptrs[i] + 0x38);
            fi_start += P38_HDR_SKIP * 2;               // skip sub-header
            idx_cnt   = (idx_cnt > P38_HDR_SKIP) ? idx_cnt - P38_HDR_SKIP : 0;
        }
        fi_starts[i] = fi_start;
        fi_ends[i]   = fi_start + idx_cnt * 2;
    }

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
                m[c][r] = rd<float>(d, base + (c*4+r)*4); // col-major: file[col][row]
        model->bind_pose[i] = m;
    }

    const uint32_t STRIDE = 32;
    for (uint32_t si = 0; si < sm_cnt; ++si) {
        uint32_t ptr = sm_ptrs[si];
        if (ptr + 0x60 > sz) continue;

        uint32_t mat_ptr = rd<uint32_t>(d, ptr);
        std::string mat_name = (mat_ptr+32 < sz) ? read_mat_name(d, mat_ptr) : "sub"+std::to_string(si);

        // Texture name: +0x24 is the shader/material type ("character", "smsimple", "smshiny", etc.)
        // When +0x24 is one of those generic types, the real texture name is at +0x44.
        std::string tex_name;
        if (mat_ptr + 0x64 <= sz) {
            auto read_str = [&](size_t off) {
                const char* s = reinterpret_cast<const char*>(d + mat_ptr + off);
                return std::string(s, strnlen(s, 28));
            };
            std::string t24 = read_str(0x24);
            bool generic = t24.empty() || t24.rfind("sm", 0) == 0 || t24 == "character" || t24.rfind("material", 0) == 0;
            tex_name = generic ? read_str(0x44) : t24;
        }

        uint32_t vc       = rd<uint32_t>(d, ptr + 0x40);
        uint32_t vo       = rd<uint32_t>(d, ptr + 0x44);
        uint32_t stride   = rd<uint32_t>(d, ptr + 0x50);
        uint32_t fi_start = fi_starts[si];
        uint32_t fi_end   = fi_ends[si];

        if (vc == 0) continue;
        if (stride != 24 && stride != 28 && stride != 32 && stride != 36 && stride != 40 && stride != 48)
            stride = STRIDE;
        if (vo + vc * stride > sz) continue;

        uint32_t n_idx = (fi_ends[si] - fi_starts[si]) / 2;

        // UV offset varies by stride:
        //   stride=24: xyz(12) + uv(8) + packed(4)
        //   stride=32: xyz(12) + packed(4) + uv(8) + bone(8)
        //   stride=36: xyz(12) + normal(12) + uv(8) + ...
        uint32_t uv_off = (stride == 24) ? 12u : (stride == 36) ? 24u : 16u;

        // ── Bone palette: count at ptr+0x08, offset at ptr+0x0c ──
        uint32_t pal_cnt = rd<uint32_t>(d, ptr + 0x08);
        uint32_t pal_off = rd<uint32_t>(d, ptr + 0x0c);
        std::vector<int> palette(pal_cnt, -1);
        if (pal_cnt > 0 && pal_cnt <= 64 && pal_off + pal_cnt*2 <= sz) {
            for (uint32_t pi = 0; pi < pal_cnt; ++pi)
                palette[pi] = (int)rd<uint16_t>(d, pal_off + pi*2);
        }

        uint32_t prim_type = rd<uint32_t>(d, ptr + 0x28);

        XBXSubmesh sm;
        sm.mat_name  = mat_name;
        sm.tex_name  = tex_name;
        sm.prim_type = prim_type;

        sm.positions.resize(vc);
        sm.uvs.resize(vc);
        sm.bone_indices.resize(vc, glm::ivec4(-1));
        sm.bone_weights.resize(vc, glm::vec4(0.f));

        for (uint32_t vi = 0; vi < vc; ++vi) {
            size_t base = vo + vi * stride;
            sm.positions[vi] = { rd<float>(d,base+0), rd<float>(d,base+4), rd<float>(d,base+8) };
            sm.uvs[vi]       = { rd<float>(d,base+uv_off), rd<float>(d,base+uv_off+4) };

            // Bone data only present when stride >= 28 and palette exists
            if (stride >= 28 && pal_cnt > 0) {
                float wsum = 0.f;
                for (int k = 0; k < 4; ++k) {
                    uint8_t local_idx = d[base + 24 + k];
                    uint8_t wt_byte   = d[base + 28 + k];
                    float   wt        = wt_byte / 255.f;
                    if (local_idx == 255 || wt == 0.f) {
                        sm.bone_indices[vi][k] = 0;
                        sm.bone_weights[vi][k] = 0.f;
                    } else {
                        int global = (local_idx < (int)palette.size()) ? palette[local_idx] : -1;
                        sm.bone_indices[vi][k] = (global >= 0 && global < N_BONES) ? global : 0;
                        sm.bone_weights[vi][k] = wt;
                        wsum += wt;
                    }
                }
                if (wsum > 0.001f)
                    sm.bone_weights[vi] /= wsum;
                else
                    sm.bone_weights[vi] = glm::vec4(1,0,0,0);
            } else {
                sm.bone_indices[vi] = glm::ivec4(0,0,0,0);
                sm.bone_weights[vi] = glm::vec4(1,0,0,0);
            }
        }

        // ptr+0x28: 5=trilist, 6=tstrip, 8=quadlist.
        // p38 path: OOB values are DMA batch dividers — filter then decode as trilist.
        if (n_idx >= 3 && fi_starts[si] > 0 && fi_ends[si] <= sz) {
            const uint16_t* raw = reinterpret_cast<const uint16_t*>(d + fi_starts[si]);
            if (fi_from_p38[si]) {
                std::vector<uint16_t> clean;
                clean.reserve(n_idx);
                for (size_t k = 0; k < n_idx; ++k)
                    if (raw[k] < vc) clean.push_back(raw[k]);
                sm.indices = trilist_to_list(clean.data(), clean.size(), vc);
            } else if (prim_type == 5) {
                sm.indices = trilist_to_list(raw, n_idx, vc);
            } else if (prim_type == 8) {
                sm.indices = quadlist_to_list(raw, n_idx, vc);
            } else {
                sm.indices = strip_to_list(raw, n_idx, vc);
            }
        } else if (n_idx == 0 && vc >= 3) {
            // No explicit index buffer — generate sequential indices based on prim type
            if (prim_type == 8) {
                // Sequential quad list: every 4 verts = 1 quad
                for (uint32_t i = 0; i + 3 < vc; i += 4) {
                    sm.indices.push_back(i);   sm.indices.push_back(i+1); sm.indices.push_back(i+2);
                    sm.indices.push_back(i);   sm.indices.push_back(i+2); sm.indices.push_back(i+3);
                }
            } else {
                // Sequential triangle strip
                for (uint32_t i = 0; i + 2 < vc; ++i) {
                    uint32_t a=i, b=i+1, c=i+2;
                    if (i&1) { sm.indices.push_back(a); sm.indices.push_back(c); sm.indices.push_back(b); }
                    else     { sm.indices.push_back(a); sm.indices.push_back(b); sm.indices.push_back(c); }
                }
            }
        }
        // Save raw u16 indices for prim-type re-interpretation in UI
        if (fi_starts[si] > 0 && fi_ends[si] <= sz) {
            size_t raw_n = (fi_ends[si] - fi_starts[si]) / 2;
            const uint16_t* rp = reinterpret_cast<const uint16_t*>(d + fi_starts[si]);
            sm.raw_indices.assign(rp, rp + raw_n);
        }
        if (!sm.indices.empty())
            model->submeshes.push_back(std::move(sm));
    }

    if (model->submeshes.empty()) { delete model; return nullptr; }
    return model;
}