void App::build_anim_bone_map(const AnimClip& clip) {
    int n_anim = clip.n_bones > 0 ? clip.n_bones : (int)clip.track_count / 3;
    m_anim_bone_map.resize(n_anim, -1);

    int n_skel = m_skeleton ? (int)m_skeleton->bones.size() : 0;
    for (int i = 0; i < n_anim; ++i) {
        if (i < (int)clip.bone_indices.size()) {
            int si = clip.bone_indices[i];
            m_anim_bone_map[i] = (si < n_skel) ? si : -1;
        } else {
            m_anim_bone_map[i] = (i < n_skel) ? i : -1;
        }
    }
}





void App::ensure_global_root_ref() {
    if (m_anim_global_ref_set) return;
    m_anim_global_ref_set = true;
    m_anim_root_ref = glm::quat(1, 0, 0, 0);
    if (m_full_rest_pose.empty() || m_anim_clips.empty()) return;

    auto upper = [](std::string s){ for (auto& c : s) if (c >= 'a' && c <= 'z') c -= 32; return s; };
    int idle = -1;
    for (int i = 0; i < (int)m_anim_clips.size(); ++i)
        if (upper(m_anim_clips[i].name).find("IDL") != std::string::npos) { idle = i; break; }
    if (idle < 0) idle = 0; 

    AnimClip& ic = m_anim_clips[idle];
    if (!ic.loaded) parse_animation(ic, m_skel_meta);
    ic.rest_pose.resize(ic.n_bones);
    for (int ai = 0; ai < ic.n_bones; ++ai) {
        int si = (ai < (int)ic.bone_indices.size()) ? ic.bone_indices[ai] : ai;
        ic.rest_pose[ai] = (si >= 0 && si < (int)m_full_rest_pose.size())
                         ? m_full_rest_pose[si] : glm::quat(1, 0, 0, 0);
    }
    ic.frames_decoded = false;
    ic.cached_frames.clear();
    auto p0 = ic.sample_pose(0.f);
    if (!p0.empty()) m_anim_root_ref = safe_quat(p0[0].q);
}

void App::select_animation(int idx) {
    if (idx < 0 || idx >= (int)m_anim_clips.size()) {
        m_anim_sel  = -1;
        m_anim_play = false;
        if (m_has_bones) {
            for (int i = 0; i < N_BONES; ++i) m_cur_pose[i] = m_bind_pose[i];
            upload_skinning();
        }
        return;
    }

    m_anim_sel  = idx;
    m_anim_time = 0.f;
    m_anim_play = true;
    m_ui_state.anim_time = 0.f;
    m_ui_state.anim_playing = true;

    AnimClip& clip = m_anim_clips[idx];
    if (!clip.loaded) parse_animation(clip, m_skel_meta);
    m_ui_state.anim_dur = clip.duration > 0 ? clip.duration
                        : (clip.fps > 0 && clip.frame_count > 1
                           ? (float)(clip.frame_count - 1) / clip.fps : 0.f);
    if (!m_full_rest_pose.empty()) {
        clip.rest_pose.resize(clip.n_bones);
        for (int ai = 0; ai < clip.n_bones; ++ai) {
            int si = (ai < (int)clip.bone_indices.size()) ? clip.bone_indices[ai] : ai;
            clip.rest_pose[ai] = (si >= 0 && si < (int)m_full_rest_pose.size())
                               ? m_full_rest_pose[si]
                               : glm::quat(1,0,0,0);
        }
        clip.frames_decoded = false;  
        clip.cached_frames.clear();
        clip.cached_root_orientations.clear();
    }

    if (m_skeleton) build_anim_bone_map(clip);

    
    
    ensure_global_root_ref();

    apply_animation_pose(0.f);
    m_last_frame = glfwGetTime();
}

void App::apply_animation_pose(float t) {
    if (!m_has_bones) return;
    if (m_anim_sel < 0 || m_anim_sel >= (int)m_anim_clips.size()) return;

    const AnimClip& clip = m_anim_clips[m_anim_sel];
    if (!clip.loaded || !m_skeleton) return;

    for (int i = 0; i < N_BONES; ++i) m_cur_pose[i] = m_bind_pose[i];

    auto poses = clip.sample_pose(t);
    int n_anim = (int)poses.size();
    int n_skel = (int)m_skeleton->bones.size();

    
    
    std::vector<glm::mat4> local_bind(n_skel, glm::mat4(1.0f));
    for (int i = 0; i < n_skel && i < N_BONES; ++i) {
        int par = m_skeleton->bones[i].parent;
        local_bind[i] = (par >= 0 && par < N_BONES)
                      ? glm::inverse(m_bind_pose[par]) * m_bind_pose[i]
                      : m_bind_pose[i];
    }

    std::vector<BonePose> pose_by_skel(n_skel);
    std::vector<bool> has_pose(n_skel, false);
    for (int ai = 0; ai < n_anim; ++ai) {
        int si = (ai < (int)m_anim_bone_map.size()) ? m_anim_bone_map[ai] : ai;
        if (si < 0 || si >= n_skel || si >= N_BONES) continue;
        pose_by_skel[si] = poses[ai];
        has_pose[si] = true;
    }

    for (int i = 0; i < n_skel && i < N_BONES; ++i) {
        int par = m_skeleton->bones[i].parent;
        glm::vec4 local_t = local_bind[i][3];
        glm::quat bind_q = safe_quat(glm::quat_cast(glm::mat3(local_bind[i])));
        glm::quat local_q = bind_q;

        if (has_pose[i]) {
            BonePose bp = pose_by_skel[i];
            float q_mag2 = bp.q.x*bp.q.x + bp.q.y*bp.q.y + bp.q.z*bp.q.z + bp.q.w*bp.q.w;
            if (std::isfinite(q_mag2) && q_mag2 >= 1e-8f) {
                glm::quat anim_q = safe_quat(bp.q);
                if (par < 0) {
                    
                    
                    
                    
                    
                    
                    local_q = safe_quat((bind_q * glm::inverse(safe_quat(m_anim_root_ref))) * anim_q);
                } else {
                    
                    
                    
                    
                    local_q = anim_q;
                }
            }
        }

        if (m_minion_lizard_model) {
            float anim_weight = 1.0f;
            switch (i) {
                case 10: case 22:
                    anim_weight = 0.80f;
                    break;
                case 11: case 12: case 13: case 14:
                case 15: case 16: case 17: case 18:
                case 23: case 24: case 25: case 26:
                case 27: case 28: case 29: case 30:
                    anim_weight = 0.45f;
                    break;
                case 32: case 38:
                    anim_weight = 0.70f;
                    break;
                case 33: case 34: case 35: case 36:
                case 39: case 40: case 41: case 42:
                    anim_weight = 0.62f;
                    break;
                default:
                    break;
            }
            if (anim_weight < 1.0f) {
                if (glm::dot(bind_q, local_q) < 0.0f) local_q = -local_q;
                local_q = safe_quat(glm::slerp(bind_q, local_q, anim_weight));
            }
        }

        glm::mat4 local_mat = glm::mat4_cast(local_q);
        local_mat[3] = local_t;

        if (par >= 0 && par < N_BONES)
            m_cur_pose[i] = m_cur_pose[par] * local_mat;
        else
            m_cur_pose[i] = local_mat;
    }

    upload_skinning();
    if (m_skeleton) {
        for (int i = 0; i < N_BONES && i < (int)m_skeleton->bones.size(); ++i)
            m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);
        rebuild_skel_gpu();
    }
}

void App::tick_animation(double now) {
    if (!m_anim_play || m_anim_sel < 0) {
        m_last_frame = now;
        return;
    }

    double dt = now - m_last_frame;
    m_last_frame = now;
    if (dt > 0.1) dt = 0.1; 

    const AnimClip& clip = m_anim_clips[m_anim_sel];
    float dur = clip.duration > 0 ? clip.duration
              : (clip.fps > 0 && clip.frame_count > 1
                 ? (float)(clip.frame_count - 1) / clip.fps : 1.f);

    m_anim_time += (float)dt;
    if (dur > 0.0f) {
        if (clip.looping) {
            m_anim_time = std::fmod(m_anim_time, dur);
            if (m_anim_time < 0.0f) m_anim_time += dur;
        } else if (m_anim_time >= dur) {
            m_anim_time = dur;
            m_anim_play = false;
            m_ui_state.anim_playing = false;
        }
    }
    m_ui_state.anim_time  = m_anim_time;
    m_ui_state.anim_dur   = dur;

    apply_animation_pose(m_anim_time);
}
