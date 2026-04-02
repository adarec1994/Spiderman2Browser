#pragma once
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct BonePose {
    glm::quat q = glm::quat(1, 0, 0, 0);
    glm::mat4 to_matrix() const;
};

struct AnimClip {
    std::string name;
    std::string path;
    bool        loaded      = false;
    bool        looping     = false;
    float       duration    = 0.f;
    float       fps         = 30.f;
    int         frame_count = 0;
    int         track_count = 0;
    int         n_bones     = 24;
    float       qscale      = 0.f;

    struct Section {
        int frame_start = 0;
        int frame_count = 0;
        int n_active    = 0;
        std::vector<uint8_t> codec_x, codec_y, codec_z;
        std::vector<uint8_t> bitstream;
        size_t bitstream_bit_offset = 0;
    };
    std::vector<Section> sections;

    // Rest pose quaternions (one per anim bone, indexed 0..n_bones-1)
    // Must be set before calling sample_pose for correct results.
    std::vector<glm::quat> rest_pose;

    std::vector<BonePose> sample_pose(float t) const;

    mutable bool frames_decoded = false;
    mutable std::vector<std::vector<BonePose>> cached_frames;
    void decode_all_frames() const;
};

// Load rest-pose quaternions from a skeleton .dat file.
// Returns 60 quaternions (one per skeleton bone), or empty on failure.
// Offset 0x2400, stride 16 bytes (x,y,z,w floats).
std::vector<glm::quat> load_skeleton_rest_pose(const std::string& skel_path);

std::vector<AnimClip> scan_animations(const std::string& folder);
void parse_animation(AnimClip& clip);