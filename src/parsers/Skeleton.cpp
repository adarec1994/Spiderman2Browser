#include "Skeleton.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cstdint>

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

Skeleton* parse_skeleton(const std::string& skel_path, const std::string& xbx_path) {
    auto skel = read_file(skel_path);
    auto xbx  = read_file(xbx_path);
    if (skel.empty() || xbx.empty()) return nullptr;

    const uint8_t* s = skel.data();
    const uint8_t* x = xbx.data();
    const size_t   ssz = skel.size();
    const size_t   xsz = xbx.size();

    // Bone table: starts at 0x03ac, stride 0x30
    // Each entry: name[0x1c], flag[4], idx[4], something[4], parent_signed[4], hash[4]
    constexpr size_t   BONE_START  = 0x03ac;
    constexpr size_t   BONE_STRIDE = 0x30;
    constexpr size_t   MAX_BONES   = 60;

    if (BONE_START + BONE_STRIDE > ssz) return nullptr;

    std::vector<Bone> bones;
    size_t off = BONE_START;
    while (off + BONE_STRIDE <= ssz && bones.size() < MAX_BONES) {
        const char* raw = reinterpret_cast<const char*>(s + off);
        if (!raw[0] || !(isalpha(raw[0]) || raw[0] == '_')) break;
        size_t namelen = strnlen(raw, 0x1c);
        std::string name(raw, namelen);
        // trim any leading junk bytes (some names have garbage prefix)
        while (!name.empty() && !(isalpha(name[0]) || name[0]=='_'))
            name = name.substr(1);
        if (name.empty()) break;

        int32_t parent = rd<int32_t>(s, off + 0x28);

        Bone b;
        b.name   = name;
        b.parent = parent;
        b.world_pos = {0,0,0};
        bones.push_back(b);
        off += BONE_STRIDE;
    }

    if (bones.empty()) return nullptr;
    size_t nb = bones.size();

    // Bind-pose world positions from XBX: 60 row-major 4x4 float matrices at 0x2f0
    // Translation = row 3 (indices 12,13,14)
    constexpr size_t MAT_BASE = 0x2f0;
    for (size_t i = 0; i < nb; ++i) {
        size_t moff = MAT_BASE + i * 64;
        if (moff + 64 > xsz) break;
        float tx = rd<float>(x, moff + 48); // row3.x
        float ty = rd<float>(x, moff + 52); // row3.y
        float tz = rd<float>(x, moff + 56); // row3.z
        bones[i].world_pos = {tx, ty, tz};
    }

    // Build line list (parent index as stored is just the list index)
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
