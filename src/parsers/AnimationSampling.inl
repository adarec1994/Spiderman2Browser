void AnimClip::decode_all_frames() const {
    if (frames_decoded) return;
    frames_decoded = true;
    cached_frames.assign(frame_count, std::vector<BonePose>(n_bones));
    cached_root_orientations.assign(frame_count, glm::quat(1, 0, 0, 0));

    
    for (int f = 0; f < frame_count; f++)
        for (int bi = 0; bi < n_bones; bi++) {
            if (bi < (int)rest_pose.size())
                cached_frames[f][bi].q = rest_pose[bi];
            if (bi < (int)rest_positions.size())
                cached_frames[f][bi].t = rest_positions[bi];
        }

    for (auto& sec : sections) {
        if (sec.frame_count <= 0) continue;

        int nf = sec.frame_count;

        if (!sec.root_quat_packet.empty()) {
            auto decoded = decode_entropy_quat_packet(sec.root_quat_packet, nf, root_q_scale * qscale);
            for (int f = 0; f < nf && f < (int)decoded.size(); ++f) {
                int abs_f = sec.frame_start + f;
                glm::quat q = game_quat_to_glm(normalized_or_identity(decoded[f]));
                if (abs_f < frame_count)
                    cached_root_orientations[abs_f] = q;
                if (abs_f < frame_count && n_bones > 0)
                    cached_frames[abs_f][0].q = q;
            }
        }

        if (BC_ENABLE_ROOT_POSITION &&
            n_bones > 0 &&
            !sec.root_pos_packets[0].empty() &&
            !sec.root_pos_packets[1].empty() &&
            !sec.root_pos_packets[2].empty()) {
            auto xs = decode_entropy_float_packet(sec.root_pos_packets[0], nf, root_pos_scale * qscale);
            auto ys = decode_entropy_float_packet(sec.root_pos_packets[1], nf, root_pos_scale * qscale);
            auto zs = decode_entropy_float_packet(sec.root_pos_packets[2], nf, root_pos_scale * qscale);
            if (!xs.empty() && !ys.empty() && !zs.empty()) {
                for (int f = 0; f < nf; ++f) {
                    int abs_f = sec.frame_start + f;
                    if (abs_f >= frame_count) continue;
                    glm::vec3 pos(xs[f], ys[f], zs[f]);
                    if (std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z) &&
                        glm::dot(pos, pos) < 100.0f) {
                        cached_frames[abs_f][0].t = pos;
                        cached_frames[abs_f][0].has_translation = true;
                    }
                }
            }
        }

        int nq = (int)std::min(sec.quat_packets.size(), sec.quat_bones.size());
        for (int qi = 0; qi < nq; qi++) {
            int bone = sec.quat_bones[qi];
            if (bone < 0 || bone >= n_bones) continue;
            
            
            
            
            
            
            if (nf >= 2 && sec.quat_packets[qi].empty()) continue;
            float track_scale = (qi < (int)sec.quat_scales.size()) ? sec.quat_scales[qi] : 0.001f;
            
            
            
            
            
            
            auto decoded = decode_entropy_quat_packet(sec.quat_packets[qi], nf, track_scale * qscale, false);

            for (int f = 0; f < nf && f < (int)decoded.size(); f++) {
                int abs_f = sec.frame_start + f;
                if (abs_f < frame_count)
                    cached_frames[abs_f][bone].q =
                        game_quat_to_glm(normalized_or_identity(decoded[f]));
            }
        }

        int np = (int)std::min(sec.pos_packets.size(), sec.pos_bones.size());
        for (int pi = 0; pi < np; ++pi) {
            int bone = sec.pos_bones[pi];
            if (bone < 0 || bone >= n_bones) continue;
            float track_scale = (pi < (int)sec.pos_scales.size()) ? sec.pos_scales[pi] : BC_POSITION_SCALE;
            const auto& packets = sec.pos_packets[pi];
            auto xs = decode_entropy_float_packet(packets[0], nf, track_scale);
            auto ys = decode_entropy_float_packet(packets[1], nf, track_scale);
            auto zs = decode_entropy_float_packet(packets[2], nf, track_scale);
            for (int f = 0; f < nf; ++f) {
                int abs_f = sec.frame_start + f;
                if (abs_f >= frame_count) continue;
                cached_frames[abs_f][bone].t = glm::vec3(xs[f], ys[f], zs[f]);
                cached_frames[abs_f][bone].has_translation = true;
            }
        }
    }

}

std::vector<BonePose> AnimClip::sample_pose(float t) const {
    if (!loaded || sections.empty()) return std::vector<BonePose>(n_bones);
    decode_all_frames();

    if (frame_count <= 0 || cached_frames.empty()) return std::vector<BonePose>(n_bones);
    if (frame_count == 1) return cached_frames[0];

    float dur = duration;
    if (dur <= 0.0f) {
        dur = fps > 0.0f ? (looping ? (float)frame_count / fps
                                    : (float)(frame_count - 1) / fps)
                         : 0.0f;
    }
    if (dur <= 0.0f) return cached_frames[0];

    float clamped = looping ? std::fmod(t, dur) : std::clamp(t, 0.f, dur);
    if (clamped < 0) clamped += dur;

    float frame_pos = looping
                    ? clamped * (float)frame_count / dur
                    : clamped * (float)(frame_count - 1) / dur;
    if (!std::isfinite(frame_pos)) frame_pos = 0.0f;

    int f0 = std::clamp((int)std::floor(frame_pos), 0, frame_count - 1);
    int f1 = looping ? (f0 + 1) % frame_count : std::min(f0 + 1, frame_count - 1);
    float alpha = std::clamp(frame_pos - (float)f0, 0.0f, 1.0f);
    if (f0 == f1 || alpha <= 0.0f) return cached_frames[f0];

    std::vector<BonePose> out(n_bones);
    int count = std::min({n_bones, (int)cached_frames[f0].size(), (int)cached_frames[f1].size()});
    for (int i = 0; i < count; ++i) {
        const BonePose& a = cached_frames[f0][i];
        const BonePose& b = cached_frames[f1][i];
        glm::quat qb = b.q;
        if (glm::dot(a.q, qb) < 0.0f) qb = -qb;
        out[i].q = glm::normalize(glm::slerp(a.q, qb, alpha));
        if (a.has_translation || b.has_translation) {
            out[i].t = glm::mix(a.t, b.t, alpha);
            out[i].has_translation = true;
        } else {
            out[i].t = a.t;
        }
    }
    for (int i = count; i < n_bones; ++i)
        out[i] = cached_frames[f0][i];
    return out;
}

glm::quat AnimClip::sample_root_orientation(float t) const {
    if (!loaded || sections.empty()) return glm::quat(1, 0, 0, 0);
    decode_all_frames();

    if (frame_count <= 0 || cached_root_orientations.empty()) return glm::quat(1, 0, 0, 0);
    if (frame_count == 1) return cached_root_orientations[0];

    float dur = duration;
    if (dur <= 0.0f) {
        dur = fps > 0.0f ? (looping ? (float)frame_count / fps
                                    : (float)(frame_count - 1) / fps)
                         : 0.0f;
    }
    if (dur <= 0.0f) return cached_root_orientations[0];

    float clamped = looping ? std::fmod(t, dur) : std::clamp(t, 0.f, dur);
    if (clamped < 0) clamped += dur;

    float frame_pos = looping
                    ? clamped * (float)frame_count / dur
                    : clamped * (float)(frame_count - 1) / dur;
    if (!std::isfinite(frame_pos)) frame_pos = 0.0f;

    int f0 = std::clamp((int)std::floor(frame_pos), 0, frame_count - 1);
    int f1 = looping ? (f0 + 1) % frame_count : std::min(f0 + 1, frame_count - 1);
    float alpha = std::clamp(frame_pos - (float)f0, 0.0f, 1.0f);
    if (f0 == f1 || alpha <= 0.0f) return cached_root_orientations[f0];

    glm::quat a = cached_root_orientations[f0];
    glm::quat b = cached_root_orientations[f1];
    if (glm::dot(a, b) < 0.0f) b = -b;
    return normalized_or_identity(glm::slerp(a, b, alpha));
}
