#include "WorldParser.h"
#include "Vfs.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   // serves from a mounted .iso or the real FS
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

// Find a 4x4 row-major float transform matrix in the range [start, start+range).
// Returns the offset within the buffer, or 0 if not found.
static size_t find_transform(const uint8_t* d, size_t sz, size_t start, size_t range) {
    size_t end = std::min(start + range, sz);
    for (size_t off = start; off + 64 <= end; off += 4) {
        float w = rf(d, off + 60);
        if (w != 1.0f) continue;
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = rf(d, off + i*4);
        bool ok = true;
        for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) { ok = false; break; }
        if (!ok) continue;
        if (std::fabs(m[12]) + std::fabs(m[14]) < 10.f) continue;
        if (!plausible_world(m[12]) || !plausible_world(m[13]) || !plausible_world(m[14])) continue;
        float r0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
        if (r0 < 0.5f || r0 > 2.0f) continue;
        return off;
    }
    return 0;
}

// Row-major (DirectX) -> GLM column-major
// DX row-major and GL column-major share the same flat memory layout for TRS matrices.
// A direct copy is correct; transposing produces wrong results.
static glm::mat4 rowmaj_to_glm(const float* m) {
    glm::mat4 r;
    memcpy(&r[0][0], m, 16 * sizeof(float));
    return r;
}

// Scan a record for a plausible XBX asset name (lowercase, has underscore, no spaces).
static std::string find_asset_name(const uint8_t* d, size_t sz, size_t from, size_t to) {
    to = std::min(to, sz);
    for (size_t off = from; off + 32 <= to; off += 4) {
        size_t name_off = off + 4;
        if (name_off + 4 > to) break;
        uint8_t c0 = d[name_off];
        if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) continue;
        std::string s = rstr(d, sz, name_off, 28);
        if (s.size() < 4) continue;
        bool valid = true;
        for (char c : s) if (c == ' ' || c == '/' || c == '\\' || c == '#') { valid = false; break; }
        if (!valid) continue;
        if (s.find('_') == std::string::npos && s.find_first_of("0123456789") == std::string::npos) continue;
        static const char* skip[] = { "SIMPLE", "COMPLEX", "STATIC", nullptr };
        bool skipped = false;
        for (int i = 0; skip[i]; ++i) if (s == skip[i]) { skipped = true; break; }
        if (skipped) continue;
        return s;
    }
    return {};
}

WorldData* parse_world(const std::string& path) {
    auto buf = read_file(path);
    if (buf.size() < 64) return nullptr;
    const uint8_t* d  = buf.data();
    const size_t   sz = buf.size();

    auto* wd = new WorldData();
    wd->source_path = path;

    // ── Named instances: scan for 0xad5bce7a marker ──────────────────────────
    static const uint8_t MARKER[4] = { 0xad, 0x5b, 0xce, 0x7a };
    for (size_t i = 0; i + 0x200 <= sz; ++i) {
        if (memcmp(d + i, MARKER, 4) != 0) continue;
        size_t rec = i;
        std::string name = rstr(d, sz, rec + 0x10, 32);
        if (name.empty() || !std::isalpha((unsigned char)name[0])) continue;
        { bool ok = true; for (char ch : name) if (!std::isalnum((unsigned char)ch) && ch != '_') { ok=false; break; } if (!ok) continue; }
        size_t mat_off = find_transform(d, sz, rec + 0xa0, 0x180);
        if (mat_off == 0) continue;
        float m[16];
        for (int k = 0; k < 16; ++k) m[k] = rf(d, mat_off + k*4);
        std::string asset = find_asset_name(d, sz, rec + 0x10 + 32, rec + 0x300);
        WorldInstance inst;
        inst.name       = name;
        inst.asset_name = asset;
        inst.transform  = rowmaj_to_glm(m);
        wd->instances.push_back(std::move(inst));
        i += 0x100;
    }

    // ── Post-process: propagate correct asset names across instance chains ────
    // Instance records form linked chains. find_asset_name often picks up the
    // NEXT instance's name instead of the real mesh reference because the search
    // window overlaps the next record. Only the first record of each type has
    // the correct asset embedded.
    // Fix: a "real" asset is one whose name does NOT appear as any instance name.
    // Group instances by base name (strip trailing digits) and propagate.
    {
        std::unordered_set<std::string> inst_names;
        for (auto& inst : wd->instances)
            inst_names.insert(inst.name);

        // base_type → correct asset (first real one found wins)
        std::unordered_map<std::string, std::string> base_asset;
        for (auto& inst : wd->instances) {
            if (inst.asset_name.empty()) continue;
            std::string base = inst.name;
            while (!base.empty() && std::isdigit((unsigned char)base.back()))
                base.pop_back();
            if (base_asset.count(base)) continue; // already have one
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

    // ── Prop type table: runs of [4B hash][28B lowercase_name] ───────────────
    // Prop type table: entries of [hash(4)+name(28)], stride 32.
    // Real prop names END in digits: "sg_stor_15d000", "blg30x1000", etc.
    // Find the LARGEST such run to avoid false positives.
    // NOTE: no underscore requirement — building meshes (blg*, apt*, roof*) lack them.
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
            // Must end in at least one digit (real asset names: "sg_stor_15d000")
            if (!std::isdigit((unsigned char)s.back())) break;
            ++count; off += 32;
        }
        if (count > prop_table_count) {
            prop_table_off   = i;
            prop_table_count = count;
        }
    }

    if (prop_table_count > 0) {
        // Use the full prop table: trees, bushes, stoops, signs, vents, awnings,
        // alley details, fire escapes, storefronts, lamps, buildings -- all of it.
        // type_idx from placement records indexes directly into this flat list.
        for (int i = 0; i < prop_table_count; ++i)
            wd->prop_types.push_back(rstr(d, sz, prop_table_off + i * 32 + 4, 28));

        // Placement records start after the full prop table.
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
            wd->props.push_back({ (int)type_idx, (float)yaw_raw, x, y, z });
            off += STRIDE - 4;
        }
    }

    return wd;
}
