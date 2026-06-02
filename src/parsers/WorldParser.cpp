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

// Locate the 4x4 row-major world transform inside an instance record.
//
// The .dat is a serialized heap image (reflective format), so the matrix does
// NOT sit at a fixed offset from the 0x7ace5bad marker — it floats (manholes at
// marker+0xD0, lampposts/trafflites at +0xE0, etc.). Rather than guess an offset,
// detect the matrix by its STRUCTURE: a genuine rotation+translation has
//   - bottom-right element == 1.0,
//   - rows 0..2 form an ORTHONORMAL 3x3 basis (each unit length, mutually
//     perpendicular).
// That signature is strong enough to find exactly one match per renderable
// instance and to naturally REJECT non-mesh logical markers (spawn points,
// lights, roof-access nodes) which carry no such matrix — so they don't render.
// Verified across A01/C15/F48: every mesh instance matches, every logical marker
// does not. Returns the matrix offset, or 0 if none in [start, start+range).
static size_t find_transform(const uint8_t* d, size_t sz, size_t start, size_t range) {
    size_t end = std::min(start + range, sz);
    auto dot3 = [](const float* a, const float* b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    };
    for (size_t off = start; off + 64 <= end; off += 4) {
        if (rf(d, off + 60) != 1.0f) continue;                 // m[3][3] == 1
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = rf(d, off + i*4);
        bool finite = true;
        for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) { finite = false; break; }
        if (!finite) continue;
        // rows 0..2 = the 3x3 basis (row r at m[r*4 .. r*4+2])
        const float* r0 = m + 0; const float* r1 = m + 4; const float* r2 = m + 8;
        float l0 = dot3(r0, r0), l1 = dot3(r1, r1), l2 = dot3(r2, r2);
        if (l0 < 0.9f || l0 > 1.1f) continue;                  // each row unit length
        if (l1 < 0.9f || l1 > 1.1f) continue;
        if (l2 < 0.9f || l2 > 1.1f) continue;
        if (std::fabs(dot3(r0, r1)) > 0.05f) continue;         // mutually orthogonal
        if (std::fabs(dot3(r0, r2)) > 0.05f) continue;
        if (std::fabs(dot3(r1, r2)) > 0.05f) continue;
        // translation (row 3) must be a sane world coordinate
        if (!plausible_world(m[12]) || !plausible_world(m[13]) || !plausible_world(m[14]))
            continue;
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

// True for the quality/LOD tag strings that sit beside the asset ref in a record.
static bool is_quality_tag(const std::string& s) {
    return s == "SIMPLE" || s == "COMPLEX" || s == "STATIC" ||
           s == "simple" || s == "complex" || s == "static";
}

// Scan an instance record's tail for the asset (mesh) reference token.
//
// Verified from real .dat data, the asset name appears in one of two casings,
// always adjacent to the SIMPLE/COMPLEX/STATIC quality tag:
//   - lowercase with trailing digits, e.g. "s_manholeb000"
//   - UPPERCASE with no trailing digits, e.g. "S_TRFFLITEA", "S_STRTLAMPB"
// The previous scanner assumed a fixed [hash:4][name] framing and stepped by 4,
// which missed the UPPERCASE form (S_TRFFLITEA) → resolution fell through to a
// fuzzy prefix search that picked the WRONG variant (a/b/c/d). That was the
// "models use the wrong/LOD-looking mesh" symptom. Here we do a plain ASCII
// token walk and return the first token that looks like an asset name and is
// NOT the quality tag.
static std::string find_asset_name(const uint8_t* d, size_t sz, size_t from, size_t to) {
    to = std::min(to, sz);
    std::string cur;
    auto accept = [&](const std::string& s) -> bool {
        if (s.size() < 4) return false;
        if (is_quality_tag(s)) return false;
        for (char c : s)
            if (c == ' ' || c == '/' || c == '\\' || c == '#' || c == ':' || c == '.')
                return false;
        // must be an identifier: letters/digits/underscore, starting with a letter
        if (!std::isalpha((unsigned char)s[0])) return false;
        for (char c : s)
            if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        // asset refs contain an underscore (s_manholeb, sa_awning, a_hydrant) or
        // trailing digits (blg30x1000); plain words don't.
        if (s.find('_') == std::string::npos &&
            s.find_first_of("0123456789") == std::string::npos)
            return false;
        return true;
    };
    for (size_t off = from; off < to; ++off) {
        uint8_t c = d[off];
        if (c >= 32 && c < 127) {
            cur.push_back((char)c);
            if (cur.size() > 28) cur.clear();   // names are <=28 chars
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

    // ── Named instances: scan for 0xad5bce7a marker ──────────────────────────
    static const uint8_t MARKER[4] = { 0xad, 0x5b, 0xce, 0x7a };
    for (size_t i = 0; i + 0x200 <= sz; ++i) {
        if (memcmp(d + i, MARKER, 4) != 0) continue;
        size_t rec = i;
        std::string name = rstr(d, sz, rec + 0x10, 32);
        if (name.empty() || !std::isalpha((unsigned char)name[0])) continue;
        { bool ok = true; for (char ch : name) if (!std::isalnum((unsigned char)ch) && ch != '_') { ok=false; break; } if (!ok) continue; }
        // The transform floats between the name and the next marker; scan from just
        // after the 32-byte name. Window 0x180 covers the observed range
        // (matrix at marker+0xD0..+0xE0+). No match => a non-mesh logical marker
        // (spawn point / light / roof-access) — skip it (nothing to render).
        size_t mat_off = find_transform(d, sz, rec + 0x30, 0x180);
        if (mat_off == 0) continue;
        float m[16];
        for (int k = 0; k < 16; ++k) m[k] = rf(d, mat_off + k*4);
        // Asset ref lives in the tail AFTER the 64-byte matrix (searching across
        // the matrix risks matching ASCII-looking float bytes). Bound the search
        // at the next marker if one is closer than 0x300.
        size_t asset_from = mat_off + 64;
        size_t asset_to   = std::min(rec + 0x300, sz);
        std::string asset = find_asset_name(d, sz, asset_from, asset_to);
        WorldInstance inst;
        inst.name       = name;
        inst.asset_name = asset;
        inst.transform  = rowmaj_to_glm(m);
        wd->instances.push_back(std::move(inst));
        i += 0x80;   // records are >=~0x150 apart; step modestly to avoid missing any
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

    // ── Building material atlas: S_BLG_BLGMASTER / PEDMASTER / S_BLG_TRM ──────
    // Building blocks (smlego shader) reference a shared master material; the cell
    // .dat lists the real wall/edge textures it maps to, each tagged with a "slot"
    // byte. Layout (verified via IDA + bytes): the master name, then a header with
    // a u8 entry-count at name_field+0x24, then COUNT entries of FIXED 36-byte
    // stride: texture-name at entry+0, slot byte = the last non-zero byte of the
    // 36. (The building's per-face slot bytes live in its instance/placement record
    // and index this table; for the viewer we expose the whole table so the
    // renderer can pick a representative wall texture instead of rendering gray.)
    {
        static const char* MASTERS[] = { "S_BLG_BLGMASTER", "S_BLG_PEDMASTER", "S_BLG_TRM", nullptr };
        for (int mi = 0; MASTERS[mi]; ++mi) {
            const std::string master = MASTERS[mi];
            // find the master name in the buffer
            size_t mpos = std::string::npos;
            for (size_t i = 0; i + master.size() < sz; ++i) {
                if (d[i] != (uint8_t)master[0]) continue;
                if (rstr(d, sz, i, master.size() + 1) == master) { mpos = i; break; }
            }
            if (mpos == std::string::npos) continue;
            // entry count is a byte at master_name + 0x24
            size_t cnt_off = mpos + 0x24;
            if (cnt_off >= sz) continue;
            int count = d[cnt_off];
            if (count <= 0 || count > 64) continue;
            // entries begin at the first lowercase ASCII run after the header
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
                // slot byte = last non-zero byte within the 36-byte record
                int slot = 0;
                for (size_t k = ESTRIDE - 1; k >= 16; --k) { if (d[base + k]) { slot = d[base + k]; break; } }
                std::transform(tex.begin(), tex.end(), tex.begin(), ::tolower);
                wd->blg_textures.push_back({ master_lc, slot, tex });
            }
        }
    }

    return wd;
}
