#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

// Per-track record: 9 bytes [type:1][float32 f0:4][float32 f1:4]
//
// CONFIRMED FORMAT (RE analysis):
//   f0 = frame-0 axis-angle component. Always the correct decoded float.
//   f1 = delta RANGE for variable tracks (NOT the frame-1 value).
//        Constant tracks: f1 == 0 (bone does not move).
//        Variable tracks: f1 = motion range; actual per-frame keyframes are
//        NAL_Packed8EntropyFloat3 entropy-coded in extra_kf_data (decode TODO).
//
// Tracks grouped 3 per bone: [bone*3+0]=X [+1]=Y [+2]=Z.
// Axis-angle vec3: direction=rotation axis, magnitude=angle in radians.
// Matches FUN_0037c200 (FSIN/FCOS on magnitude).
struct TrackRecord {
    uint8_t type;   // NAL Golomb-Rice k parameter
    float   f0;     // frame-0 component (confirmed correct float)
    float   f1;     // motion range (0=constant; non-zero=animated, decode TODO)
};

// Per-animated-bone axis-angle vector (xyz = axis * angle in radians)
struct BonePose {
    float ax, ay, az;
    glm::mat4 to_matrix() const;  // Rodrigues, matches FUN_0037c200
};

struct AnimClip {
    std::string name;
    std::string path;

    float    duration    = 0.f;
    float    fps         = 30.f;
    int      frame_count = 0;
    bool     loop        = false;
    uint32_t track_count = 0;   // tc; n_animated_bones = tc/3

    // NAL codec params (per character class): stored at 0xb8 and 0xbc
    uint32_t bits_per_comp = 8;    // b8 (8 for BC, 22-131 for COP, 27 for CIV)
    uint32_t quant_range   = 4064; // bc

    // Decoded base track records (size = track_count).
    // f0 = confirmed correct frame-0 rotation component.
    // f1 = motion range (non-zero identifies variable/animated tracks).
    std::vector<TrackRecord> tracks;

    // Number of variable tracks (= tail.size(), always 6 in observed data).
    int n_variable_tracks = 0;

    // Raw entropy-coded keyframe deltas (NAL_Packed8Entropy, decode TODO).
    std::vector<uint8_t> extra_kf_data;

    // Tail: [4, N_frames] per variable track, at end of stream.
    struct TailEntry { uint32_t bytes_per_val; uint32_t n_frames; };
    std::vector<TailEntry> tail;

    bool loaded = false;

    // Sample pose at time t (seconds).
    // Returns one BonePose per bone (size = track_count/3).
    // Constant tracks: exact f0. Variable tracks: f0 (keyframe decode TODO).
    std::vector<BonePose> sample_pose(float t) const;
};

std::vector<AnimClip> scan_animations(const std::string& folder);
bool parse_animation(AnimClip& clip);
