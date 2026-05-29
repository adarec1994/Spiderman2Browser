#pragma once
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct BonePose {
    glm::quat q = glm::quat(1, 0, 0, 0);
    glm::vec3 t = glm::vec3(0.0f);
    bool has_translation = false;
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
    int         n_bones     = 0;
    float       qscale      = 0.f;
    float       root_pos_scale = 0.001f; // ae_base_bone position scale (scale_table[0])
    float       root_q_scale   = 0.001f; // ae_base_bone orientation scale (scale_table[1])
    std::vector<int> bone_indices;

    struct Section {
        int frame_start = 0;
        int frame_count = 0;
        int n_active    = 0;
        std::vector<uint8_t> root_quat_packet;
        std::array<std::vector<uint8_t>, 3> root_pos_packets;
        std::vector<std::vector<uint8_t>> quat_packets;
        std::vector<int> quat_bones;
        std::vector<float> quat_scales;
        std::vector<uint8_t> floor_packet;
        std::array<std::vector<uint8_t>, 4> trajectory_packets;
        std::vector<std::vector<uint8_t>> signal_packets;
        int skipped_position_tracks = 0;
        int skipped_extra_packets = 0;
        std::vector<std::array<std::vector<uint8_t>, 3>> pos_packets;
        std::vector<int> pos_bones;
        std::vector<float> pos_scales;
    };
    std::vector<Section> sections;

    // Rest pose quaternions (one per animated rotation track, indexed 0..n_bones-1)
    // Must be set before calling sample_pose for correct results.
    std::vector<glm::quat> rest_pose;
    std::vector<glm::vec3> rest_positions;

    std::vector<BonePose> sample_pose(float t) const;
    glm::quat sample_root_orientation(float t) const;

    mutable bool frames_decoded = false;
    mutable std::vector<std::vector<BonePose>> cached_frames;
    mutable std::vector<glm::quat> cached_root_orientations;
    void decode_all_frames() const;
};

// Per-skeleton animation metadata, located dynamically in the skeleton .dat
// (a named-chunk container) so any character works, not just Black Cat. The
// bone table, rest-pose quats, and per-source scale table live at different
// offsets in every skeleton; they're found by structure, not hardcoded.
struct SkeletonAnimMeta {
    bool  valid       = false;
    int   bone_count  = 0;                 // total bones incl. root (pelvis)
    std::vector<glm::quat> rest_pose;      // bone_count quats, game->GL converted (root = identity)
    float root_pos_scale = 0.001f;         // scale_table[0]
    float root_q_scale   = 0.001f;         // scale_table[1]
    std::vector<float> quat_scales;        // bone_count-1 nal_quaternion track scales (scale_table[2..])
};
// bone_count comes from parse_skeleton (the bone table); pass it so the rest
// pose and scale table are read with the right length.
SkeletonAnimMeta load_skeleton_meta(const std::string& skel_path, int bone_count);

std::vector<AnimClip> scan_animations(const std::string& folder);
void parse_animation(AnimClip& clip, const SkeletonAnimMeta& meta);
