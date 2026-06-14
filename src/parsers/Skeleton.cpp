#include "Skeleton.h"
#include "XBXParser.h"
#include "Vfs.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cstdint>

static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}


static bool valid_bone_name(const uint8_t* p, size_t avail) {
    if (avail < 4) return false;
    char c0 = (char)p[0];
    if (!(isalpha((unsigned char)c0) || c0 == '_')) return false;
    for (size_t i = 0; i < avail && i < 0x1c; ++i) {
        if (p[i] == 0) return i >= 3;
        if (p[i] < 32 || p[i] > 126) return false;
    }
    return true;
}




static size_t find_bone_table(const uint8_t* s, size_t ssz, size_t stride) {
    for (size_t off = 0x40; off + 4 * stride <= ssz && off < 0x1000; ++off) {
        int ok = 0;
        for (int k = 0; k < 4; ++k)
            if (valid_bone_name(s + off + k * stride, ssz - (off + k * stride))) ok++;
            else break;
        if (ok >= 4) return off;
    }
    return 0;
}

Skeleton* parse_skeleton(const std::string& skel_path, const std::string& xbx_path) {
    auto skel = read_file(skel_path);
    auto xbx  = read_file(xbx_path);
    if (skel.empty() || xbx.empty()) return nullptr;

    const uint8_t* s = skel.data();
    const uint8_t* x = xbx.data();
    const size_t   ssz = skel.size();
    const size_t   xsz = xbx.size();

    
    
    constexpr size_t   BONE_STRIDE = 0x30;
    constexpr size_t   MAX_BONES   = 60;

    size_t BONE_START = find_bone_table(s, ssz, BONE_STRIDE);
    if (BONE_START == 0 || BONE_START + BONE_STRIDE > ssz) return nullptr;

    std::vector<Bone> bones;
    size_t off = BONE_START;
    while (off + BONE_STRIDE <= ssz && bones.size() < MAX_BONES) {
        const char* raw = reinterpret_cast<const char*>(s + off);
        if (!raw[0] || !(isalpha(raw[0]) || raw[0] == '_')) break;
        size_t namelen = strnlen(raw, 0x1c);
        std::string name(raw, namelen);
        
        while (!name.empty() && !(isalpha(name[0]) || name[0]=='_'))
            name = name.substr(1);
        if (name.empty()) break;

        int32_t parent = rd<int32_t>(s, off + 0x28);

        
        
        
        
        
        
        
        
        
        
        
        
        
        
        constexpr int MAX_BONE_INDEX = 256; 
        if (parent < -1 || parent >= MAX_BONE_INDEX) break;

        Bone b;
        b.name   = name;
        b.parent = parent;
        b.world_pos = {0,0,0};
        bones.push_back(b);
        off += BONE_STRIDE;
    }

    if (bones.empty()) return nullptr;
    size_t nb = bones.size();

    
    
    
    
    
    
    int mat_count = 0;
    const size_t MAT_BASE = xbx_find_bind_matrix_base(x, xsz, &mat_count);
    for (size_t i = 0; i < nb; ++i) {
        
        
        if (mat_count > 0 && (int)i >= mat_count) break;
        size_t moff = MAT_BASE + i * 64;
        if (moff + 64 > xsz) break;
        float tx = rd<float>(x, moff + 48); 
        float ty = rd<float>(x, moff + 52); 
        float tz = rd<float>(x, moff + 56); 
        bones[i].world_pos = {tx, ty, tz};
    }

    
    std::vector<std::pair<int,int>> lines;
    for (size_t i = 0; i < nb; ++i) {
        int p = bones[i].parent;
        if (p >= 0 && p < (int)nb)
            lines.push_back({p, (int)i});
    }

    auto* sk = new Skeleton();
    sk->bones = std::move(bones);
    sk->lines = std::move(lines);
    return sk;
}
