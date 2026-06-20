static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}





static int count_substr(const uint8_t* d, size_t sz, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || sz < nlen) return 0;
    int n = 0;
    for (size_t i = 0; i + nlen <= sz; ) {
        if (memcmp(d + i, needle, nlen) == 0) { ++n; i += nlen; }
        else ++i;
    }
    return n;
}





static constexpr int BC_QUAT_TRACK_COUNT = 59;
static const float BC_QUAT_SCALE[BC_QUAT_TRACK_COUNT] = {
    0.00100000f, 0.00100000f, 0.00100000f, 0.00100000f,
    0.00781250f, 0.00781250f, 0.00781250f, 0.00100000f,
    0.00100000f, 0.00781250f, 0.01562500f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.00100000f, 0.00100000f,
    0.00100000f, 0.00100000f, 0.00781250f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
    0.01562500f, 0.01562500f, 0.01562500f, 0.00100000f,
    0.00100000f, 0.00100000f, 0.00100000f, 0.00100000f,
    0.00195312f, 0.00195312f, 0.00195312f, 0.00100000f,
    0.00195312f, 0.00195312f, 0.00195312f,
};




static constexpr float BC_ROOT_POSITION_SOURCE_SCALE = 0.00100000f;
static constexpr float BC_ROOT_Q_SCALE = 0.00100000f;





static const int BC_POSITION_BONES[] = {
    1, 2, 3, 4, 5, 6, 7, 8
};
static constexpr float BC_POSITION_SCALE = 0.00100000f;
static constexpr bool BC_ENABLE_POSITION_TRACKS = false;
static constexpr bool BC_ENABLE_ROOT_POSITION = true;
static const float BC_ROOT_POSITION_SCALE[3] = {
    BC_ROOT_POSITION_SOURCE_SCALE,
    BC_ROOT_POSITION_SOURCE_SCALE,
    BC_ROOT_POSITION_SOURCE_SCALE
};

glm::mat4 BonePose::to_matrix() const { return glm::mat4_cast(q); }

static glm::quat game_quat_to_glm(const glm::quat& q);








static size_t find_rest_quat_run(const uint8_t* d, size_t sz, int want) {
    if (want < 1) return 0;
    if (want > 8) want = 8;            
    for (size_t o = 0x40; o + 16 * (size_t)want <= sz; o += 4) {
        int ok = 0;
        for (int k = 0; k < want; ++k) {
            float q[4]; memcpy(q, d + o + (size_t)k * 16, 16);
            float m = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
            if (m > 0.95f && m < 1.05f) ok++; else break;
        }
        if (ok >= want) return o;
    }
    return 0;
}


static size_t find_scale_table(const uint8_t* d, size_t sz, int& out_len) {
    size_t best_off = 0; int best_len = 0;
    for (size_t o = 0x100; o + 4 <= sz; o += 4) {
        int n = 0;
        while (o + (size_t)(n + 1) * 4 <= sz) {
            float v; memcpy(&v, d + o + (size_t)n * 4, 4);
            if (v >= 0.0001f && v <= 0.25f) n++; else break;
        }
        if (n > best_len) { best_len = n; best_off = o; }
        if (n > 1) o += (size_t)(n - 1) * 4;   
    }
    out_len = best_len;
    return best_off;
}

SkeletonAnimMeta load_skeleton_meta(const std::string& skel_path, int bone_count) {
    SkeletonAnimMeta meta;
    if (bone_count <= 1) return meta;
    auto data = read_file(skel_path);
    if (data.empty()) return meta;
    const uint8_t* d = data.data();
    size_t sz = data.size();
    meta.bone_count = bone_count;

    
    
    meta.rest_pose.assign(bone_count, glm::quat(1, 0, 0, 0));
    size_t rq = find_rest_quat_run(d, sz, bone_count - 1);
    if (rq) {
        for (int b = 1; b < bone_count; ++b) {
            size_t off = rq + (size_t)(b - 1) * 16;
            if (off + 16 > sz) break;
            glm::quat q(rd<float>(d, off + 12), rd<float>(d, off),
                        rd<float>(d, off + 4), rd<float>(d, off + 8));
            float m2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
            if (!std::isfinite(m2) || m2 < 0.25f || m2 > 4.0f) q = glm::quat(1, 0, 0, 0);
            else q = q * (1.0f / std::sqrt(m2));
            meta.rest_pose[b] = game_quat_to_glm(q);
        }
    }

    
    int slen = 0;
    size_t so = find_scale_table(d, sz, slen);
    meta.quat_scales.assign(bone_count - 1, 0.001f);
    if (so && slen >= bone_count + 1) {
        meta.root_pos_scale = rd<float>(d, so);
        meta.root_q_scale   = rd<float>(d, so + 4);
        for (int i = 0; i < bone_count - 1; ++i)
            meta.quat_scales[i] = rd<float>(d, so + (size_t)(2 + i) * 4);
    }

    
    
    
    
    
    meta.quat_track_count = count_substr(d, sz, "nal_quaternion");

    meta.valid = (rq != 0);
    return meta;
}




struct BitReader {
    const uint8_t* data; size_t len, bp; int bi;
    BitReader(const uint8_t* d, size_t n, size_t bo = 0, int bio = 0)
        : data(d), len(n), bp(bo), bi(bio) {}
    uint32_t read(int n) {
        uint32_t v = 0; int g = 0;
        while (g < n) { if (bp >= len) return v; int a = 8-bi, t = std::min(a, n-g);
        v |= (uint32_t)((data[bp]>>bi)&((1<<t)-1))<<g; g+=t; bi+=t; if (bi>=8){bi=0;bp++;}} return v; }
    uint32_t peek(int n) { auto sb=bp; auto si=bi; auto v=read(n); bp=sb; bi=si; return v; }
    size_t tell() const { return bp*8+bi; }
};
