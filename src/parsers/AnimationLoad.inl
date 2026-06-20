void parse_animation(AnimClip& clip, const SkeletonAnimMeta& meta) {
    auto data = read_file(clip.path);
    if (data.size() < 0x120) { clip.loaded = false; return; }
    const uint8_t* d = data.data();
    size_t sz = data.size();
    if (rd<uint32_t>(d, 0) != 0x00010101) { clip.loaded = false; return; }

    clip.looping     = rd<uint32_t>(d, 0xA4) != 0;
    clip.duration    = rd<float>(d, 0xA8);
    clip.fps         = rd<float>(d, 0xB0);
    clip.frame_count = rd<uint32_t>(d, 0xB4);
    uint32_t ref_size  = rd<uint32_t>(d, 0xC4);
    uint32_t sec_count = rd<uint32_t>(d, 0xD4);
    uint32_t max_fps   = rd<uint32_t>(d, 0xD8);
    clip.qscale = rd<float>(d, 0x104);

    size_t sec_table = 0x100 + ref_size;
    if (sec_table + sec_count * 4 > sz) { clip.loaded = false; return; }

    std::vector<uint32_t> sec_offsets(sec_count);
    for (uint32_t i = 0; i < sec_count; i++)
        sec_offsets[i] = rd<uint32_t>(d, sec_table + i * 4);

    size_t sec_data_base = sec_table + sec_count * 4;

    clip.n_bones = (meta.valid && meta.bone_count > 1) ? meta.bone_count : 60;
    clip.root_pos_scale = meta.valid ? meta.root_pos_scale : BC_ROOT_POSITION_SOURCE_SCALE;
    clip.root_q_scale   = meta.valid ? meta.root_q_scale   : BC_ROOT_Q_SCALE;
    clip.bone_indices.resize(clip.n_bones);
    for (int i = 0; i < clip.n_bones; ++i) clip.bone_indices[i] = i;
    clip.track_count = clip.n_bones * 3;
    clip.sections.resize(sec_count);
    clip.frames_decoded = false;
    clip.cached_frames.clear();
    clip.cached_root_orientations.clear();

    for (uint32_t si = 0; si < sec_count; si++) {
        size_t sec_start = sec_data_base + sec_offsets[si];
        size_t sec_end = (si + 1 < sec_count) ? sec_data_base + sec_offsets[si + 1] : sz;
        if (sec_start >= sz) continue;
        int frames_in_sec = (int)std::min((uint32_t)(clip.frame_count - si * max_fps), max_fps);
        if (frames_in_sec <= 0) continue;

        auto& sec = clip.sections[si];
        sec.frame_start = (int)(si * max_fps);
        sec.frame_count = frames_in_sec;
        sec.root_quat_packet.clear();
        for (auto& p : sec.root_pos_packets) p.clear();
        sec.quat_packets.clear();
        sec.quat_bones.clear();
        sec.quat_scales.clear();
        sec.floor_packet.clear();
        for (auto& p : sec.trajectory_packets) p.clear();
        sec.signal_packets.clear();
        sec.skipped_position_tracks = 0;
        sec.skipped_extra_packets = 0;
        sec.pos_packets.clear();
        sec.pos_bones.clear();
        sec.pos_scales.clear();

        size_t cur = sec_start;

        auto read_packet = [&](std::vector<uint8_t>& out) -> bool {
            out.clear();
            if (cur >= sec_end || cur >= sz) return false;
            uint8_t stream_len = d[cur++];
            if (cur + stream_len > sec_end || cur + stream_len > sz) {
                cur = sec_end;
                return false;
            }
            out.assign(d + cur, d + cur + stream_len);
            cur += stream_len;
            return true;
        };

        
        
        for (int axis = 0; axis < 3; ++axis)
            read_packet(sec.root_pos_packets[axis]);
        read_packet(sec.root_quat_packet);

        
        
        
        
        
        
        
        int q_track_count = (meta.quat_track_count > 0)
                          ? meta.quat_track_count
                          : ((meta.valid && meta.bone_count > 1)
                             ? meta.bone_count - 1 : BC_QUAT_TRACK_COUNT);
        for (int qi = 0; qi < q_track_count && cur < sec_end && cur < sz; ++qi) {
            std::vector<uint8_t> packet;
            if (!read_packet(packet)) break;
            sec.quat_packets.push_back(std::move(packet));
            sec.quat_bones.push_back(qi + 1);
            float s = (meta.valid && qi < (int)meta.quat_scales.size())
                    ? meta.quat_scales[qi]
                    : (qi < BC_QUAT_TRACK_COUNT ? BC_QUAT_SCALE[qi] : 0.001f);
            if (meta.quat_effective_scale_cap > 0.0f &&
                clip.qscale != 0.0f) {
                float effective = s * clip.qscale;
                if (effective > meta.quat_effective_scale_cap)
                    s = meta.quat_effective_scale_cap / clip.qscale;
            }
            sec.quat_scales.push_back(s);
        }

        
        
        
        
        read_packet(sec.floor_packet);
        for (int ti = 0; ti < 4; ++ti)
            read_packet(sec.trajectory_packets[ti]);

        std::vector<std::vector<uint8_t>> tail_packets;
        while (cur < sec_end && cur < sz) {
            std::vector<uint8_t> packet;
            if (!read_packet(packet)) break;
            tail_packets.push_back(std::move(packet));
        }

        int signal_count = (int)(tail_packets.size() % 3);
        if (signal_count > 6) signal_count = 0;
        for (int si_tail = 0; si_tail < signal_count; ++si_tail)
            sec.signal_packets.push_back(std::move(tail_packets[si_tail]));

        int pos_packet_start = signal_count;
        int available_pos_tracks = (int)(tail_packets.size() - pos_packet_start) / 3;
        int max_known_pos_tracks = (int)(sizeof(BC_POSITION_BONES) / sizeof(BC_POSITION_BONES[0]));
        int pos_track_count = BC_ENABLE_POSITION_TRACKS
                            ? std::min(available_pos_tracks, max_known_pos_tracks)
                            : 0;
        for (int pi = 0; pi < pos_track_count; ++pi) {
            std::array<std::vector<uint8_t>, 3> packets;
            for (int axis = 0; axis < 3; ++axis)
                packets[axis] = std::move(tail_packets[pos_packet_start + pi * 3 + axis]);
            sec.pos_packets.push_back(std::move(packets));
            sec.pos_bones.push_back(BC_POSITION_BONES[pi]);
            sec.pos_scales.push_back(BC_POSITION_SCALE);
        }
        sec.skipped_position_tracks = available_pos_tracks - pos_track_count;
        sec.skipped_extra_packets = (int)tail_packets.size()
                                  - signal_count
                                  - available_pos_tracks * 3;

        sec.n_active = (sec.root_pos_packets[0].empty() ? 0 : 1)
                     + (sec.root_quat_packet.empty() ? 0 : 1)
                     + (int)sec.quat_packets.size()
                     + (int)sec.pos_packets.size();
    }

    clip.loaded = true;
}

std::vector<AnimClip> scan_animations(const std::string& folder) {
    std::vector<AnimClip> clips;
    namespace fs = std::filesystem;
    if (!vfs::is_directory(folder)) return clips;

    for (auto& entry : vfs::list_dir(folder)) {
        if (entry.is_dir) continue;
        std::string path = entry.path;
        std::string stem = fs::path(path).stem().string();
        std::string ext  = fs::path(path).extension().string();
        for (auto& c : ext) c = tolower(c);
        if (ext != ".dat") continue;
        if (stem.empty()) continue;

        
        
        
        
        std::vector<uint8_t> hdr = vfs::read_file(path);
        if (hdr.size() < 0xB8) continue;
        if (rd<uint32_t>(hdr.data(), 0) != 0x00010101) continue;

        AnimClip clip;
        clip.name = stem; clip.path = path;
        clip.looping     = rd<uint32_t>(hdr.data(), 0xA4) != 0;
        clip.duration    = rd<float>(hdr.data(), 0xA8);
        clip.fps         = rd<float>(hdr.data(), 0xB0);
        clip.frame_count = rd<uint32_t>(hdr.data(), 0xB4);
        clip.n_bones=0; clip.track_count=0;
        clips.push_back(std::move(clip));
    }
    std::sort(clips.begin(), clips.end(),
              [](const AnimClip& a, const AnimClip& b) { return a.name < b.name; });
    return clips;
}
