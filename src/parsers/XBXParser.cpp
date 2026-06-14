#include "XBXParser.h"
#include "Vfs.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <glm/gtc/matrix_transform.hpp>

static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}

static std::string read_mat_name(const uint8_t* data, size_t ptr) {
    const char* s = reinterpret_cast<const char*>(data + ptr + 4);
    return std::string(s, strnlen(s, 28));
}



static bool xbx_is_affine_mat(const uint8_t* d, size_t sz, size_t o) {
    if (o + 64 > sz) return false;
    if (std::fabs(rd<float>(d, o + 12)) > 1e-3f) return false; 
    if (std::fabs(rd<float>(d, o + 28)) > 1e-3f) return false; 
    if (std::fabs(rd<float>(d, o + 44)) > 1e-3f) return false; 
    if (std::fabs(rd<float>(d, o + 60) - 1.0f) > 1e-3f) return false; 
    for (int c = 0; c < 3; ++c) {
        float a = rd<float>(d, o + c * 16);
        float b = rd<float>(d, o + c * 16 + 4);
        float e = rd<float>(d, o + c * 16 + 8);
        float m = a * a + b * b + e * e;
        if (!(m > 0.25f && m < 4.0f)) return false; 
    }
    return true;
}







size_t xbx_find_bind_matrix_base(const uint8_t* d, size_t sz, int* out_count) {
    auto count_run = [&](size_t o) {
        int n = 0; while (xbx_is_affine_mat(d, sz, o + (size_t)n * 64)) ++n; return n;
    };
    size_t best = 0; int best_n = 0;
    for (size_t o = 0x40; o + 64 <= sz; o += 4) {
        if (!xbx_is_affine_mat(d, sz, o)) continue;
        float tx = rd<float>(d, o + 48), ty = rd<float>(d, o + 52), tz = rd<float>(d, o + 56);
        if (tx * tx + ty * ty + tz * tz > 4e-4f) continue; 
        int n = count_run(o);
        if (n >= 4) { best = o; best_n = n; break; }       
    }
    
    if (!best) {
        for (size_t o = 0x40; o + 64 <= sz; o += 4) {
            if (!xbx_is_affine_mat(d, sz, o)) continue;
            int n = count_run(o);
            if (n >= 8) { best = o; best_n = n; break; }
        }
    }
    if (!best) { best = 0x2f0; best_n = 0; }
    if (out_count) *out_count = best_n;
    return best;
}




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















static std::vector<uint32_t> pushbuffer_to_list(const uint8_t* d, size_t byte_len, uint32_t vc) {
    std::vector<uint16_t> indices;
    int prim = 6;                                   
    size_t o = 0;
    while (o + 4 <= byte_len) {
        uint32_t hdr    = rd<uint32_t>(d, o); o += 4;
        uint32_t method = hdr & 0x3FFFF;
        uint32_t count  = (hdr >> 18) & 0x7FF;      
        if (count == 0) continue;                   
        if (o + (size_t)count * 4 > byte_len) break;
        if (method == 0x17FC) {                     
            uint32_t p = rd<uint32_t>(d, o) & 0xFFFF;
            if (p != 0) prim = (int)p;              
        } else if (method == 0x1800) {              
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t dw = rd<uint32_t>(d, o + (size_t)k * 4);
                indices.push_back((uint16_t)(dw & 0xFFFF));
                indices.push_back((uint16_t)(dw >> 16));
            }
        }
        o += (size_t)count * 4;
    }
    if (indices.empty()) return {};
    const uint16_t* idx = indices.data();
    size_t n = indices.size();
    if (prim == 5)      return trilist_to_list(idx, n, vc);
    else if (prim == 8) return quadlist_to_list(idx, n, vc);
    else                return strip_to_list(idx, n, vc);
}

XBXModel* parse_xbx(const std::string& filepath, bool primary_geom_only) {
    auto buf = read_file(filepath);
    if (buf.size() < 16) return nullptr;
    const uint8_t* d = buf.data();
    const size_t   sz = buf.size();

    if (memcmp(d, "XBXM", 4) != 0) return nullptr;

    uint32_t hdr_sz    = rd<uint32_t>(d, 0x0c);

    
    
    
    
    
    
    
    struct MatInfo { std::string shader, tex; };
    std::unordered_map<uint32_t, MatInfo> mat_map;
    {
        uint32_t n_chunks = rd<uint32_t>(d, 0x08);
        auto read_entry = [&](uint32_t ptr) -> std::string {
            if (ptr == 0 || ptr + 4 >= sz) return {};
            const char* s = reinterpret_cast<const char*>(d + ptr + 4);
            return std::string(s, strnlen(s, 28));
        };
        for (uint32_t ci = 0; ci < n_chunks; ++ci) {
            uint32_t base  = hdr_sz + ci * 12;
            if (base + 12 > sz) break;
            uint32_t ctype = rd<uint32_t>(d, base);
            uint32_t desc  = rd<uint32_t>(d, base + 4);
            uint32_t matp  = rd<uint32_t>(d, base + 8);
            if ((ctype & 0xFF000000) == 0x02000000) continue; 
            if (desc == 0 || desc + 28 > sz || matp == 0) continue;
            uint32_t shader_ptr = rd<uint32_t>(d, desc + 4);  
            uint32_t tex_ptr    = rd<uint32_t>(d, desc + 24); 
            mat_map[matp] = { read_entry(shader_ptr), read_entry(tex_ptr) };
        }
    }

    
    
    
    
    std::vector<uint32_t> geom_bases;
    {
        uint32_t n_ch = rd<uint32_t>(d, 0x08);
        for (uint32_t ci = 0; ci < n_ch; ++ci) {
            uint32_t base  = hdr_sz + ci * 12;
            if (base + 12 > sz) break;
            uint32_t ctype = rd<uint32_t>(d, base);
            uint32_t ptr   = rd<uint32_t>(d, base + 4);
            if ((ctype >> 24) != 0x02) continue;
            uint32_t gb = ptr + 4;
            if (gb + 0x48 > sz) continue;
            uint32_t smc = rd<uint32_t>(d, gb + 0x04);
            if (smc == 0 || smc > 64) continue;
            geom_bases.push_back(gb);
        }
    }
    
    if (geom_bases.empty()) {
        uint32_t chunk_cnt = rd<uint32_t>(d, 0x08);
        for (uint32_t ci = 0; ci < chunk_cnt; ++ci) {
            uint32_t base = hdr_sz + ci * 0x30;
            if (base + 12*4 > sz) break;
            for (uint32_t i = 0; i < 12; ++i) {
                uint32_t v = rd<uint32_t>(d, base + i*4);
                if ((v & 0xFF000000) == 0x02000000 && i + 1 < 12) {
                    uint32_t gb = rd<uint32_t>(d, base + (i+1)*4) + 4;
                    if (gb + 0x48 <= sz) {
                        uint32_t smc = rd<uint32_t>(d, gb + 0x04);
                        if (smc > 0 && smc <= 64)
                            geom_bases.push_back(gb);
                    }
                    break;
                }
            }
            if (!geom_bases.empty()) break;
        }
    }
    if (geom_bases.empty()) return nullptr;

    
    
    
    
    
    if (primary_geom_only && geom_bases.size() > 1) {
        uint32_t best_gb = geom_bases[0]; uint32_t best_v = 0;
        for (uint32_t gb : geom_bases) {
            uint32_t smc = rd<uint32_t>(d, gb + 0x04);
            if (smc == 0 || smc > 64) continue;
            uint32_t vsum = 0;
            for (uint32_t i = 0; i < smc; ++i) {
                uint32_t sp = rd<uint32_t>(d, gb + 0x40 + i * 8);
                if (sp + 0x44 <= sz) vsum += rd<uint32_t>(d, sp + 0x40);
            }
            if (vsum > best_v) { best_v = vsum; best_gb = gb; }
        }
        geom_bases.assign(1, best_gb);
    }

    auto* model = new XBXModel();
    model->filepath = filepath;

    
    
    int mat_count = 0;
    const size_t MAT_BASE = xbx_find_bind_matrix_base(d, sz, &mat_count);
    constexpr int    N_BONES  = 60;
    model->bind_pose.resize(N_BONES, glm::mat4(1.f));
    int bind_count = mat_count > 0 ? std::min(N_BONES, mat_count) : N_BONES;
    for (int i = 0; i < bind_count; ++i) {
        size_t base = MAT_BASE + i*64;
        if (base + 64 > sz) break;
        glm::mat4 m;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m[c][r] = rd<float>(d, base + (c*4+r)*4); 
        model->bind_pose[i] = m;
    }

    const uint32_t STRIDE = 32;
    std::string last_tex_name; 

    for (uint32_t gbi = 0; gbi < (uint32_t)geom_bases.size(); ++gbi) {
        uint32_t geom_base = geom_bases[gbi];
        uint32_t sm_cnt = rd<uint32_t>(d, geom_base + 0x04);
        if (sm_cnt == 0 || sm_cnt > 64) continue;

        std::vector<uint32_t> sm_ptrs(sm_cnt);
        for (uint32_t i = 0; i < sm_cnt; ++i)
            sm_ptrs[i] = rd<uint32_t>(d, geom_base + 0x40 + i*8);

        
        
        
        
        
        
        
        
        
        
        std::vector<uint32_t> fi_starts(sm_cnt, 0), fi_ends(sm_cnt, 0);
        std::vector<bool>     fi_from_p38(sm_cnt, false);
        for (uint32_t i = 0; i < sm_cnt; ++i) {
            
            
            
            
            
            
            
            
            
            
            if (sm_ptrs[i] + 0x40 > sz) continue;
            uint32_t p30     = rd<uint32_t>(d, sm_ptrs[i] + 0x30);
            uint32_t idx_cnt = rd<uint32_t>(d, sm_ptrs[i] + 0x2c);
            uint32_t fi_start, fi_end;
            if (p30 != 0) {
                fi_start = p30;
                fi_end   = fi_start + idx_cnt * 2;
            } else {
                fi_from_p38[i]    = true;
                fi_start          = rd<uint32_t>(d, sm_ptrs[i] + 0x38);  
                uint32_t pb_bytes = rd<uint32_t>(d, sm_ptrs[i] + 0x3c);  
                
                
                if (fi_start == 0 || fi_start >= sz || pb_bytes == 0) { fi_from_p38[i] = false; continue; }
                fi_end = (uint32_t)std::min<uint64_t>((uint64_t)fi_start + pb_bytes, (uint64_t)sz);
            }
            fi_starts[i] = fi_start;
            fi_ends[i]   = fi_end;
        }

        for (uint32_t si = 0; si < sm_cnt; ++si) {
        uint32_t ptr = sm_ptrs[si];
        if (ptr + 0x60 > sz) continue;

        uint32_t mat_ptr = rd<uint32_t>(d, ptr);
        std::string mat_name = (mat_ptr+32 < sz) ? read_mat_name(d, mat_ptr) : "sub"+std::to_string(si);

        
        
        
        
        
        std::string shader_type_str;
        std::string tex_name;
        std::vector<std::string> tex_candidates;

        auto push = [&](const std::string& s) {
            if (s.empty()) return;
            static const char* img_exts[] = { ".tga", ".bmp", ".png", ".jpg", ".jpeg", nullptr };
            std::string clean = s;
            std::string slo = s;
            std::transform(slo.begin(), slo.end(), slo.begin(), ::tolower);
            for (int i = 0; img_exts[i]; ++i) {
                size_t elen = strlen(img_exts[i]);
                if (slo.size() > elen && slo.substr(slo.size()-elen) == img_exts[i]) {
                    clean = s.substr(0, s.size()-elen);
                    break;
                }
            }
            if (clean.empty()) return;
            if (std::find(tex_candidates.begin(), tex_candidates.end(), clean) == tex_candidates.end())
                tex_candidates.push_back(clean);
        };

        auto it = mat_map.find(mat_ptr);
        if (it != mat_map.end()) {
            
            shader_type_str = it->second.shader;
            push(it->second.tex);
            
            
            
            
            
            if (mat_ptr + 0x48 <= sz) {
                const char* t44 = reinterpret_cast<const char*>(d + mat_ptr + 0x44);
                std::string s44(t44, strnlen(t44, 28));
                if (!s44.empty() && s44 != it->second.tex && s44 != mat_name &&
                    !std::isdigit((unsigned char)s44[0]) &&
                    s44.find(' ') == std::string::npos)
                    push(s44);
            }
        } else {
            
            auto is_shader = [](const std::string& s) -> bool {
                static const char* shaders[] = {
                    "sm_grunge", "smcharenv", "smcharenvmorph", "smfxenv",
                    "smglass", "smgrass", "smlego", "smlegosimple",
                    "smlowlodsimple", "smroof", "smshiny", "smsign",
                    "smsimple", "smspidey", "smstar", "smstreet",
                    "smtranslucent", "character", "charactermorph",
                    nullptr
                };
                for (int i = 0; shaders[i]; ++i)
                    if (s == shaders[i]) return true;
                if (s.rfind("sm", 0) == 0)       return true;
                if (s.rfind("material", 0) == 0)  return true;
                return false;
            };
            auto looks_like_tex = [](const std::string& s) -> bool {
                if (s.empty()) return false;
                if (std::isdigit((unsigned char)s[0])) return false;
                for (char c : s)
                    if (c == ' ' || c == '/' || c == '\\' || c == '#') return false;
                return true;
            };
            if (mat_ptr + 0x80 <= sz) {
                auto read_str = [&](size_t off) -> std::string {
                    const char* s = reinterpret_cast<const char*>(d + mat_ptr + off);
                    return std::string(s, strnlen(s, 28));
                };
                std::string t04 = read_str(0x04);
                std::string t24 = read_str(0x24);
                std::string t44 = read_str(0x44);
                if (is_shader(t24))                      shader_type_str = t24;
                if (looks_like_tex(t24) && !is_shader(t24)) push(t24);
                if (looks_like_tex(t44) && !is_shader(t44)) push(t44);
                if (looks_like_tex(t04) && !is_shader(t04)) push(t04);
            }
        }

        
        {
            std::vector<std::string> stripped;
            for (const auto& c : tex_candidates) {
                if (c.size() > 3) {
                    const auto tail = c.substr(c.size() - 3);
                    if (std::all_of(tail.begin(), tail.end(),
                                    [](char ch){ return std::isdigit((unsigned char)ch); })) {
                        std::string base = c.substr(0, c.size() - 3);
                        if (!base.empty() &&
                            std::find(tex_candidates.begin(), tex_candidates.end(), base) == tex_candidates.end())
                            stripped.push_back(base);
                    }
                }
            }
            for (auto& s : stripped) push(s);
        }

        
        {
            std::vector<std::string> ifl_stripped;
            static const std::string IFL = "_ifl";
            for (const auto& c : tex_candidates) {
                if (c.size() > IFL.size() && c.substr(c.size() - IFL.size()) == IFL) {
                    std::string base = c.substr(0, c.size() - IFL.size());
                    if (!base.empty() &&
                        std::find(tex_candidates.begin(), tex_candidates.end(), base) == tex_candidates.end())
                        ifl_stripped.push_back(base);
                }
            }
            for (auto& s : ifl_stripped) push(s);
        }

        tex_name = tex_candidates.empty() ? "" : tex_candidates[0];

        
        if (tex_candidates.empty() && !last_tex_name.empty()) {
            tex_candidates.push_back(last_tex_name);
            tex_name = last_tex_name;
        } else if (!tex_candidates.empty()) {
            last_tex_name = tex_candidates[0];
        }

        uint32_t vc       = rd<uint32_t>(d, ptr + 0x40);
        uint32_t vo       = rd<uint32_t>(d, ptr + 0x44);
        uint32_t stride   = rd<uint32_t>(d, ptr + 0x50);
        uint32_t fi_start = fi_starts[si];
        uint32_t fi_end   = fi_ends[si];

        if (vc == 0) continue;
        if (stride != 16 && stride != 20 && stride != 24 && stride != 28 &&
            stride != 32 && stride != 36 && stride != 40 && stride != 48)
            stride = STRIDE;
        if (vo + vc * stride > sz) continue;

        uint32_t n_idx = (fi_ends[si] - fi_starts[si]) / 2;

        
        
        
        
        
        
        
        
        
        
        
        auto uv_ok = [](float v) {
            if (!std::isfinite(v)) return false;
            float a = std::fabs(v);
            if (a > 64.0f) return false;          
            if (a != 0.0f && a < 1e-4f) return false; 
            return true;
        };
        uint32_t nsamp = std::min<uint32_t>(vc, 32u);
        uint32_t uv_off = 0xFFFFFFFFu;
        for (uint32_t o = 12; o + 8u <= stride; o += 4u) {
            
            
            if (o + 12u <= stride) {
                int unit = 0, tot = 0;
                for (uint32_t s = 0; s < nsamp; ++s) {
                    size_t b = vo + (size_t)s * stride;
                    float x = rd<float>(d,b+o), y = rd<float>(d,b+o+4), z = rd<float>(d,b+o+8);
                    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
                        float m = x*x + y*y + z*z; ++tot;
                        if (m > 0.92f && m < 1.08f) ++unit;
                    }
                }
                if (tot > 0 && unit >= (int)(tot * 0.8f)) { o += 8u; continue; } 
            }
            int ok = 0;
            for (uint32_t s = 0; s < nsamp; ++s) {
                size_t b = vo + (size_t)s * stride;
                if (uv_ok(rd<float>(d,b+o)) && uv_ok(rd<float>(d,b+o+4))) ++ok;
            }
            if (nsamp > 0 && ok >= (int)(nsamp * 0.9f)) { uv_off = o; break; }
        }
        if (uv_off == 0xFFFFFFFFu) {
            
            if      (stride == 36)                 uv_off = 24u;
            else if (stride == 24 || stride == 20) uv_off = 12u;
            else if (stride >= 24)                 uv_off = 16u;
            else                                   uv_off = 0u;
            if (uv_off + 8u > stride)              uv_off = 0u;
        }

        
        uint32_t pal_cnt = rd<uint32_t>(d, ptr + 0x08);
        uint32_t pal_off = rd<uint32_t>(d, ptr + 0x0c);
        std::vector<int> palette(pal_cnt, -1);
        if (pal_cnt > 0 && pal_cnt <= 64 && pal_off + pal_cnt*2 <= sz) {
            for (uint32_t pi = 0; pi < pal_cnt; ++pi)
                palette[pi] = (int)rd<uint16_t>(d, pal_off + pi*2);
        }

        uint32_t prim_type = rd<uint32_t>(d, ptr + 0x28);

        XBXSubmesh sm;
        sm.mat_name       = mat_name;
        sm.shader_type    = shader_type_str;
        sm.tex_name       = tex_name;
        sm.tex_candidates = tex_candidates;
        sm.prim_type      = prim_type;
        sm.from_pushbuffer = fi_from_p38[si];

        sm.positions.resize(vc);
        sm.uvs.resize(vc);
        sm.bone_indices.resize(vc, glm::ivec4(-1));
        sm.bone_weights.resize(vc, glm::vec4(0.f));

        for (uint32_t vi = 0; vi < vc; ++vi) {
            size_t base = vo + vi * stride;
            sm.positions[vi] = { rd<float>(d,base+0), rd<float>(d,base+4), rd<float>(d,base+8) };
            sm.uvs[vi]       = { rd<float>(d,base+uv_off), rd<float>(d,base+uv_off+4) };

            
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

        
        
        
        
        
        
        
        
        
        
        if (fi_from_p38[si] && fi_starts[si] > 0 && fi_ends[si] <= sz) {
            
            sm.indices = pushbuffer_to_list(d + fi_starts[si], fi_ends[si] - fi_starts[si], vc);
        } else if (n_idx >= 3 && fi_starts[si] > 0 && fi_ends[si] <= sz) {
            const uint16_t* raw = reinterpret_cast<const uint16_t*>(d + fi_starts[si]);
            if (prim_type == 5) {
                sm.indices = trilist_to_list(raw, n_idx, vc);
            } else if (prim_type == 8) {
                sm.indices = quadlist_to_list(raw, n_idx, vc);
            } else {
                sm.indices = strip_to_list(raw, n_idx, vc);
            }
        } else if (n_idx == 0 && vc >= 3) {
            
            if (prim_type == 8) {
                
                for (uint32_t i = 0; i + 3 < vc; i += 4) {
                    sm.indices.push_back(i);   sm.indices.push_back(i+1); sm.indices.push_back(i+2);
                    sm.indices.push_back(i);   sm.indices.push_back(i+2); sm.indices.push_back(i+3);
                }
            } else if (prim_type == 5) {
                
                for (uint32_t i = 0; i + 2 < vc; i += 3) {
                    sm.indices.push_back(i); sm.indices.push_back(i+1); sm.indices.push_back(i+2);
                }
            } else {
                
                for (uint32_t i = 0; i + 2 < vc; ++i) {
                    uint32_t a=i, b=i+1, c=i+2;
                    if (i&1) { sm.indices.push_back(a); sm.indices.push_back(c); sm.indices.push_back(b); }
                    else     { sm.indices.push_back(a); sm.indices.push_back(b); sm.indices.push_back(c); }
                }
            }
        }
        
        if (fi_starts[si] > 0 && fi_ends[si] <= sz) {
            size_t raw_n = (fi_ends[si] - fi_starts[si]) / 2;
            const uint16_t* rp = reinterpret_cast<const uint16_t*>(d + fi_starts[si]);
            sm.raw_indices.assign(rp, rp + raw_n);
        }
        if (!sm.indices.empty())
            model->submeshes.push_back(std::move(sm));
        } 
    } 

    if (model->submeshes.empty()) { delete model; return nullptr; }
    return model;
}
