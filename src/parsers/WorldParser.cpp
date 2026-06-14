#include "WorldParser.h"
#include "Vfs.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}
static float    rf(const uint8_t* p, size_t off) { return rd<float>   (p, off); }
static uint32_t ru(const uint8_t* p, size_t off) { return rd<uint32_t>(p, off); }
static uint16_t rh(const uint8_t* p, size_t off) { return rd<uint16_t>(p, off); }

static std::string rstr(const uint8_t* d, size_t sz, size_t off, size_t maxlen = 32) {
    if (off >= sz) return {};
    size_t end = off;
    while (end < sz && end < off + maxlen && d[end] != 0) ++end;
    std::string s(reinterpret_cast<const char*>(d + off), end - off);
    for (char c : s) if (c < 32 || c > 126) return {};
    return s;
}

static bool plausible_world(float v) {
    return std::isfinite(v) && std::fabs(v) < 100000.f;
}















static size_t find_transform(const uint8_t* d, size_t sz, size_t start, size_t range) {
    size_t end = std::min(start + range, sz);
    auto dot3 = [](const float* a, const float* b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    };
    for (size_t off = start; off + 64 <= end; off += 4) {
        if (rf(d, off + 60) != 1.0f) continue;                 
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = rf(d, off + i*4);
        bool finite = true;
        for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) { finite = false; break; }
        if (!finite) continue;
        
        const float* r0 = m + 0; const float* r1 = m + 4; const float* r2 = m + 8;
        float l0 = dot3(r0, r0), l1 = dot3(r1, r1), l2 = dot3(r2, r2);
        if (l0 < 0.9f || l0 > 1.1f) continue;                  
        if (l1 < 0.9f || l1 > 1.1f) continue;
        if (l2 < 0.9f || l2 > 1.1f) continue;
        if (std::fabs(dot3(r0, r1)) > 0.05f) continue;         
        if (std::fabs(dot3(r0, r2)) > 0.05f) continue;
        if (std::fabs(dot3(r1, r2)) > 0.05f) continue;
        
        if (!plausible_world(m[12]) || !plausible_world(m[13]) || !plausible_world(m[14]))
            continue;
        return off;
    }
    return 0;
}




static glm::mat4 rowmaj_to_glm(const float* m) {
    glm::mat4 r;
    memcpy(&r[0][0], m, 16 * sizeof(float));
    return r;
}


static bool is_quality_tag(const std::string& s) {
    return s == "SIMPLE" || s == "COMPLEX" || s == "STATIC" ||
           s == "simple" || s == "complex" || s == "static";
}













static std::string find_asset_name(const uint8_t* d, size_t sz, size_t from, size_t to) {
    to = std::min(to, sz);
    std::string cur;
    auto accept = [&](const std::string& s) -> bool {
        if (s.size() < 4) return false;
        if (is_quality_tag(s)) return false;
        for (char c : s)
            if (c == ' ' || c == '/' || c == '\\' || c == '#' || c == ':' || c == '.')
                return false;
        
        if (!std::isalpha((unsigned char)s[0])) return false;
        for (char c : s)
            if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        
        
        if (s.find('_') == std::string::npos &&
            s.find_first_of("0123456789") == std::string::npos)
            return false;
        return true;
    };
    for (size_t off = from; off < to; ++off) {
        uint8_t c = d[off];
        if (c >= 32 && c < 127) {
            cur.push_back((char)c);
            if (cur.size() > 28) cur.clear();   
        } else {
            if (!cur.empty()) { if (accept(cur)) return cur; cur.clear(); }
        }
    }
    if (!cur.empty() && accept(cur)) return cur;
    return {};
}

WorldData* parse_world(const std::string& path) {
    auto buf = read_file(path);
    if (buf.size() < 64) return nullptr;
    const uint8_t* d  = buf.data();
    const size_t   sz = buf.size();

    auto* wd = new WorldData();
    wd->source_path = path;

    
    static const uint8_t MARKER[4] = { 0xad, 0x5b, 0xce, 0x7a };
    for (size_t i = 0; i + 0x200 <= sz; ++i) {
        if (memcmp(d + i, MARKER, 4) != 0) continue;
        size_t rec = i;
        std::string name = rstr(d, sz, rec + 0x10, 32);
        if (name.empty() || !std::isalpha((unsigned char)name[0])) continue;
        { bool ok = true; for (char ch : name) if (!std::isalnum((unsigned char)ch) && ch != '_') { ok=false; break; } if (!ok) continue; }
        
        
        
        
        size_t mat_off = find_transform(d, sz, rec + 0x30, 0x180);
        if (mat_off == 0) continue;
        float m[16];
        for (int k = 0; k < 16; ++k) m[k] = rf(d, mat_off + k*4);
        
        
        
        size_t asset_from = mat_off + 64;
        size_t asset_to   = std::min(rec + 0x300, sz);
        std::string asset = find_asset_name(d, sz, asset_from, asset_to);
        WorldInstance inst;
        inst.name       = name;
        inst.asset_name = asset;
        inst.transform  = rowmaj_to_glm(m);
        wd->instances.push_back(std::move(inst));
        i += 0x80;   
    }

    
    
    
    
    
    
    
    {
        std::unordered_set<std::string> inst_names;
        for (auto& inst : wd->instances)
            inst_names.insert(inst.name);

        
        std::unordered_map<std::string, std::string> base_asset;
        for (auto& inst : wd->instances) {
            if (inst.asset_name.empty()) continue;
            std::string base = inst.name;
            while (!base.empty() && std::isdigit((unsigned char)base.back()))
                base.pop_back();
            if (base_asset.count(base)) continue; 
            if (inst_names.find(inst.asset_name) == inst_names.end())
                base_asset[base] = inst.asset_name;
        }
        for (auto& inst : wd->instances) {
            std::string base = inst.name;
            while (!base.empty() && std::isdigit((unsigned char)base.back()))
                base.pop_back();
            auto it = base_asset.find(base);
            if (it != base_asset.end())
                inst.asset_name = it->second;
        }
    }

    
    
    
    
    
    size_t prop_table_off   = 0;
    int    prop_table_count = 0;
    for (size_t i = 32; i + 64 <= sz; i += 4) {
        int count = 0;
        size_t off = i;
        while (off + 32 <= sz && count < 256) {
            uint8_t c0 = d[off + 4];
            if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) break;
            std::string s = rstr(d, sz, off + 4, 28);
            if (s.size() < 4) break;
            
            if (!std::isdigit((unsigned char)s.back())) break;
            ++count; off += 32;
        }
        if (count > prop_table_count) {
            prop_table_off   = i;
            prop_table_count = count;
        }
    }

    if (prop_table_count > 0) {
        
        
        
        for (int i = 0; i < prop_table_count; ++i)
            wd->prop_types.push_back(rstr(d, sz, prop_table_off + i * 32 + 4, 28));

        
        const size_t STRIDE     = 0x34;
        const size_t scan_start = prop_table_off + prop_table_count * 32;

        for (size_t off = scan_start; off + STRIDE <= sz; off += 4) {
            if (d[off] != 0x09) continue;
            if (d[off + 1] != 0x00) continue;
            uint16_t yaw_raw = rh(d, off + 2);
            if (yaw_raw > 359) continue;
            float x = rf(d, off + 4), y = rf(d, off + 8), z = rf(d, off + 12);
            if (!plausible_world(x) || !plausible_world(y) || !plausible_world(z)) continue;
            if (std::fabs(x) + std::fabs(z) < 10.f) continue;
            uint8_t type_idx = d[off + 18];
            if (type_idx >= (uint8_t)prop_table_count) continue;
            
            
            
            
            
            
            
            float bh = 0.f; int slot = -1;
            {
                std::string tl = wd->prop_types[type_idx];
                for (auto& c : tl) c = (char)std::tolower((unsigned char)c);
                if (tl.rfind("blg",0)==0 || tl.rfind("apt",0)==0) {
                    uint16_t hh = rh(d, off + 32);
                    if (hh >= 4 && hh <= 500) bh = (float)hh;
                    slot = d[off + 0x2c];           
                }
            }
            wd->props.push_back({ (int)type_idx, (float)yaw_raw, x, y, z, bh, slot });
            off += STRIDE - 4;
        }
    }

    
    
    
    
    
    
    
    
    
    {
        static const char* MASTERS[] = { "S_BLG_BLGMASTER", "S_BLG_PEDMASTER", "S_BLG_TRM", nullptr };
        for (int mi = 0; MASTERS[mi]; ++mi) {
            const std::string master = MASTERS[mi];
            
            size_t mpos = std::string::npos;
            for (size_t i = 0; i + master.size() < sz; ++i) {
                if (d[i] != (uint8_t)master[0]) continue;
                if (rstr(d, sz, i, master.size() + 1) == master) { mpos = i; break; }
            }
            if (mpos == std::string::npos) continue;
            
            size_t cnt_off = mpos + 0x24;
            if (cnt_off >= sz) continue;
            int count = d[cnt_off];
            if (count <= 0 || count > 64) continue;
            
            size_t e0 = cnt_off + 1;
            while (e0 < sz && !(d[e0] >= 'a' && d[e0] <= 'z')) ++e0;
            const size_t ESTRIDE = 36;
            std::string master_lc = master;
            std::transform(master_lc.begin(), master_lc.end(), master_lc.begin(), ::tolower);
            for (int e = 0; e < count; ++e) {
                size_t base = e0 + (size_t)e * ESTRIDE;
                if (base + ESTRIDE > sz) break;
                std::string tex = rstr(d, sz, base, 28);
                if (tex.size() < 4 || !(d[base] >= 'a' && d[base] <= 'z')) continue;
                
                int slot = 0;
                for (size_t k = ESTRIDE - 1; k >= 16; --k) { if (d[base + k]) { slot = d[base + k]; break; } }
                std::transform(tex.begin(), tex.end(), tex.begin(), ::tolower);
                wd->blg_textures.push_back({ master_lc, slot, tex });
            }
        }
    }

    return wd;
}
