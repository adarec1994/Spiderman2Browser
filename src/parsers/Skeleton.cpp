#include "Skeleton.h"
#include "XBXParser.h"
#include "Vfs.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cstdint>

static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   // serves from a mounted .iso or the real FS
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}

// A bone-table entry begins with a printable, NUL-terminated name (alpha or '_').
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

// The bone table sits at a per-character offset (the skeleton .dat is a
// named-chunk container). Find it: the first place with >=4 consecutive
// stride-0x30 entries that all start with a valid bone name.
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

    // Bone table: per-character offset, stride 0x30.
    // Each entry: name[0x1c], flag[4], idx[4], something[4], parent_signed[4], hash[4]
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
        // trim any leading junk bytes (some names have garbage prefix)
        while (!name.empty() && !(isalpha(name[0]) || name[0]=='_'))
            name = name.substr(1);
        if (name.empty()) break;

        int32_t parent = rd<int32_t>(s, off + 0x28);

        // Stop at the first over-read entry. valid_bone_name alone over-reads:
        // past the real table sit string/float fragments ("ae_base_bone",
        // "ernion" from "...quaternion") that look like names but whose parent
        // field is garbage — string/float bytes reinterpreted as int32, always
        // huge magnitude (ASCII high byte => >1e9, or a float => ~±1e9). Real
        // parents are a small bone index (or -1 for a root). Phantom bones inflate
        // the count, which then mis-sizes the rest pose and the animation quat-track
        // count (q_tracks = bone_count-1) -> the pose decode shifts and the model
        // renders mangled. 60-bone rigs escaped this via the 60 clamp; smaller ones
        // (e.g. 24-deformer civilians) did not. NOTE: some rigs list bones in
        // grouped (non-hierarchical) order, so a parent may be a LATER index
        // (forward ref, e.g. the Spidey rig's "l hand" -> parent 58). Do NOT bound
        // by the current count (that truncated such rigs to ~6 bones); bound by a
        // generous max plausible bone index so only the billion-valued junk is cut.
        constexpr int MAX_BONE_INDEX = 256; // real rigs are <=64 bones; junk is ~1e9
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

    // Bind-pose world positions from the XBX bind matrices (col-major 4x4 float).
    // The array offset is per-character (variable header) — locate it, do NOT
    // hardcode 0x2f0 (that offset is only correct for 60-bone Black Cat; civilians
    // sit at e.g. 0x270/0x2b0, so a fixed read produced a scrambled skeleton).
    // Matrix i is the bind-WORLD transform of bone i (1:1); translation column
    // (file offset 48/52/56) is the bone's world position.
    int mat_count = 0;
    const size_t MAT_BASE = xbx_find_bind_matrix_base(x, xsz, &mat_count);
    for (size_t i = 0; i < nb; ++i) {
        // Only the deformer bones have matrices; the trailing fakeroot has none,
        // so stop at mat_count to avoid reading past the array into garbage.
        if (mat_count > 0 && (int)i >= mat_count) break;
        size_t moff = MAT_BASE + i * 64;
        if (moff + 64 > xsz) break;
        float tx = rd<float>(x, moff + 48); // translation.x
        float ty = rd<float>(x, moff + 52); // translation.y
        float tz = rd<float>(x, moff + 56); // translation.z
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
