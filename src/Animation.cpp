#include "Animation.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint32_t ru32(const uint8_t* p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static float rf32(const uint8_t* p) {
    float v; memcpy(&v, p, 4); return v;
}

// True if a float is a plausible axis-angle component (|v| < 2π + margin).
static bool is_plausible(float v) {
    return std::isfinite(v) && std::fabs(v) < 7.0f;
}

// ── BonePose::to_matrix ───────────────────────────────────────────────────────
// Rodrigues' rotation formula, matching FUN_0037c200 (FSIN/FCOS on magnitude).
glm::mat4 BonePose::to_matrix() const {
    float angle = std::sqrt(ax*ax + ay*ay + az*az);
    if (angle < 1e-8f) return glm::mat4(1.0f);

    float inv = 1.0f / angle;
    float nx = ax * inv, ny = ay * inv, nz = az * inv;
    float s = std::sin(angle),  c = std::cos(angle),  t = 1.0f - c;

    return glm::mat4(
        t*nx*nx + c,    t*nx*ny + s*nz, t*nx*nz - s*ny, 0,
        t*nx*ny - s*nz, t*ny*ny + c,    t*ny*nz + s*nx, 0,
        t*nx*nz + s*ny, t*ny*nz - s*nx, t*nz*nz + c,    0,
        0,              0,              0,              1
    );
}

// ── AnimClip::sample_pose ─────────────────────────────────────────────────────
std::vector<BonePose> AnimClip::sample_pose(float /*t*/) const {
    int n_bones = (int)track_count / 3;
    std::vector<BonePose> out(n_bones, {0.f, 0.f, 0.f});

    if (!loaded || tracks.empty()) return out;

    // We always return the frame-0 pose from f0.
    //
    // CONFIRMED: f0 is the correct decoded float for every track:
    //   - Constant tracks (f1==0): f0 is the static bone rotation.
    //   - Variable tracks (f1!=0): f0 is the base/frame-0 value; the
    //     per-frame keyframe deltas live in extra_kf_data as NAL_Packed8Entropy
    //     coded integers (decode TODO — requires the runtime NAL decoder).
    //
    // For 2-frame POSE files (BCSWGVERT*): f0 gives the correct swing pose.
    // For multi-frame files: f0 gives the correct rest/base pose, with
    // animated bones frozen at their frame-0 position until keyframes decoded.

    for (int bone = 0; bone < n_bones; ++bone) {
        float vals[3] = {0.f, 0.f, 0.f};
        for (int c = 0; c < 3; ++c) {
            int ti = bone * 3 + c;
            if (ti >= (int)tracks.size()) break;
            const TrackRecord& tr = tracks[ti];
            vals[c] = is_plausible(tr.f0) ? tr.f0 : 0.f;
        }
        out[bone] = {vals[0], vals[1], vals[2]};
    }
    return out;
}

// ── Header parsing ────────────────────────────────────────────────────────────

static bool parse_header(const std::vector<uint8_t>& data, AnimClip& clip) {
    if (data.size() < 0x150) return false;
    if (ru32(data.data()) != 0x00010101) return false;

    const char* name_ptr = reinterpret_cast<const char*>(data.data() + 0x14);
    clip.name = std::string(name_ptr, strnlen(name_ptr, 64));

    clip.loop          = (ru32(data.data() + 0xa4) != 0);
    clip.duration      = rf32(data.data() + 0xa8);
    clip.fps           = rf32(data.data() + 0xb0);
    clip.frame_count   = (int)ru32(data.data() + 0xb4);
    clip.bits_per_comp = ru32(data.data() + 0xb8);
    clip.quant_range   = ru32(data.data() + 0xbc);
    clip.track_count   = ru32(data.data() + 0xc4);

    // tc must be a non-zero multiple of 3 (axis-angle format)
    return clip.track_count > 0 && clip.track_count % 3 == 0;
}

// ── Full parse ────────────────────────────────────────────────────────────────

bool parse_animation(AnimClip& clip) {
    if (clip.path.empty()) return false;

    std::ifstream f(clip.path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>()
    );

    if (!parse_header(data, clip)) return false;

    const size_t stream_start = 0x14c;
    if (data.size() <= stream_start) return false;

    const uint8_t* stream    = data.data() + stream_start;
    const size_t   stream_len = data.size() - stream_start;
    const uint32_t tc         = clip.track_count;
    const size_t   base_size  = (size_t)tc * 9;

    clip.tracks.resize(tc);

    // ── Base track records: tc * 9 bytes ──────────────────────────────────
    // [uint8 type][float32 f0][float32 f1]
    // f0 = confirmed correct frame-0 value.
    // f1 = motion range (0 for constant tracks).
    if (stream_len >= base_size) {
        for (uint32_t i = 0; i < tc; ++i) {
            const uint8_t* rec = stream + i * 9;
            clip.tracks[i].type = rec[0];
            clip.tracks[i].f0   = rf32(rec + 1);
            clip.tracks[i].f1   = rf32(rec + 5);
        }
    } else {
        // Compact format (some COP files have stream_len < base_size).
        // Parse what we can; remainder stays zero-initialised.
        size_t n = stream_len / 9;
        for (size_t i = 0; i < n && i < tc; ++i) {
            const uint8_t* rec = stream + i * 9;
            clip.tracks[i].type = rec[0];
            clip.tracks[i].f0   = rf32(rec + 1);
            clip.tracks[i].f1   = rf32(rec + 5);
        }
    }

    // ── Tail descriptor: [4, N_frames] pairs at end of stream ─────────────
    // One entry per variable (animated) track.  Always 6 in observed data.
    if (stream_len >= 8) {
        int n_tail = 0;
        const uint8_t* p = stream + stream_len - 8;
        while (p >= stream + base_size) {
            uint32_t a = ru32(p);
            uint32_t b = ru32(p + 4);
            if (a == 4 && b == (uint32_t)clip.frame_count) {
                ++n_tail;
                p -= 8;
            } else {
                break;
            }
        }
        clip.n_variable_tracks = n_tail;
        clip.tail.resize(n_tail);
        p = stream + stream_len - (size_t)n_tail * 8;
        for (int i = 0; i < n_tail; ++i, p += 8)
            clip.tail[i] = { ru32(p), ru32(p + 4) };

        // ── Extra keyframe region (NAL entropy-coded deltas, decode TODO) ──
        size_t tail_off = stream_len - (size_t)n_tail * 8;
        if (tail_off > base_size) {
            size_t extra_len = tail_off - base_size;
            clip.extra_kf_data.assign(
                stream + base_size,
                stream + base_size + extra_len);
        }
    }

    clip.loaded = true;
    return true;
}

// ── scan_animations ───────────────────────────────────────────────────────────

std::vector<AnimClip> scan_animations(const std::string& folder) {
    std::vector<AnimClip> clips;
    if (folder.empty()) return clips;

    int n_dat = 0, n_ok = 0;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".dat") continue;
        ++n_dat;

        AnimClip clip;
        clip.path = entry.path().string();
        clip.name = entry.path().stem().string();

        // Quick header scan (no track data)
        std::ifstream f(clip.path, std::ios::binary);
        if (!f) continue;
        std::vector<uint8_t> hdr(0x150, 0);
        f.read(reinterpret_cast<char*>(hdr.data()), (std::streamsize)hdr.size());
        if ((size_t)f.gcount() < 0x150) continue;
        if (!parse_header(hdr, clip)) continue;

        ++n_ok;
        clips.push_back(std::move(clip));
    }

    std::cout << "scan_animations: " << n_dat << " .dat files, "
              << n_ok << " valid anim clips in " << folder << "\n";

    std::sort(clips.begin(), clips.end(),
        [](const AnimClip& a, const AnimClip& b){ return a.name < b.name; });
    return clips;
}