#include "Animation.h"
#include "Vfs.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   // serves from a mounted .iso or the real FS
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}

// Count non-overlapping occurrences of an ASCII needle in a byte blob.
// Used to read the skeleton's animation source-descriptor list, which names
// exactly one "nal_quaternion" per animated rotation track — the authoritative
// quaternion-track count (bone_count-1 over-counts by the trailing fakeroot).
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

// Black Cat pose sources are laid out like the game skeleton source table:
// source 0 is ae_base_bone (a position/orientation source for bip01 pelvis),
// sources 1..59 are nal_quaternion tracks for bones 1..59, source 60 is
// AE_Floor_Offset, and source 61 is NAL_TRAJECTORY/fakeroot.
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
// Source 0 (ae_base_bone) is a position/orientation source. The local
// translation table ends at BLACK_CAT.dat + 0x2ae0; source scales start at
// 0x2ae4. IDA's NAL position/orient writer uses scale[0] for XYZ translation
// and scale[1] for orientation.
static constexpr float BC_ROOT_POSITION_SOURCE_SCALE = 0.00100000f;
static constexpr float BC_ROOT_Q_SCALE = 0.00100000f;

// BC clips seen so far carry eight active nal_position triples after signals.
// These begin at source id 68, with source metadata targeting bones 1..59.
// These are left disabled until the packed float3 metadata path is fully
// modeled from IDA; applying them as plain local translations is wrong.
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

// ============================================================
// Skeleton-format locators (work for any character, not just Black Cat)
// ============================================================
// The skeleton .dat is a named-chunk container; the rest-pose quats and the
// per-source scale table sit at per-character offsets. Find them by structure.

// First run of >= want consecutive unit quaternions = the rest-pose table.
static size_t find_rest_quat_run(const uint8_t* d, size_t sz, int want) {
    if (want < 1) return 0;
    if (want > 8) want = 8;            // 8 is plenty to lock onto the table
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

// Longest run of small positive floats (source scales, ~1/1000..1/4) = scale table.
static size_t find_scale_table(const uint8_t* d, size_t sz, int& out_len) {
    size_t best_off = 0; int best_len = 0;
    for (size_t o = 0x100; o + 4 <= sz; o += 4) {
        int n = 0;
        while (o + (size_t)(n + 1) * 4 <= sz) {
            float v; memcpy(&v, d + o + (size_t)n * 4, 4);
            if (v >= 0.0001f && v <= 0.25f) n++; else break;
        }
        if (n > best_len) { best_len = n; best_off = o; }
        if (n > 1) o += (size_t)(n - 1) * 4;   // keep it linear
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

    // Rest pose: root (bone 0) is an identity placeholder (ae_base_bone); bones
    // 1..N-1 come from the rest-quat run.
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

    // Scale table: [root_pos, root_orient, quat_0 .. quat_(N-2), extras].
    int slen = 0;
    size_t so = find_scale_table(d, sz, slen);
    meta.quat_scales.assign(bone_count - 1, 0.001f);
    if (so && slen >= bone_count + 1) {
        meta.root_pos_scale = rd<float>(d, so);
        meta.root_q_scale   = rd<float>(d, so + 4);
        for (int i = 0; i < bone_count - 1; ++i)
            meta.quat_scales[i] = rd<float>(d, so + (size_t)(2 + i) * 4);
    }

    // Authoritative quaternion-track count: one "nal_quaternion" source per
    // animated rotation track in the skeleton's source-descriptor list. This is
    // the real count; bone_count-1 over-counts by the trailing fakeroot (e.g.
    // MINION_LIZARD: 44 nal_quaternion vs bone_count-1 = 45), making the decoder
    // read one packet too many and desync the stream tail. 0 = not found.
    meta.quat_track_count = count_substr(d, sz, "nal_quaternion");

    meta.valid = (rq != 0);
    return meta;
}

// ============================================================
// BitReader
// ============================================================
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

static inline int32_t sar(int32_t v, int s) { return v >> s; }
static void dec_zeros(int32_t* o, int n) { for (int i = 0; i < n; i++) o[i] = 0; }

// ============================================================
// Entropy codecs (all 62 functions)
// ============================================================
static int dec_one(BitReader& br, int rem, int codec, int32_t* out) {
    int32_t b, v, low2, low3;
    switch (codec) {
    case 1:
        b=br.peek(2); if(!(b&1)){br.read(1);out[0]=0;return 1;}
        br.read(2);out[0]=(b&3)-2;return 1;
    case 2:
        b=br.peek(4); if(!(b&1)){br.read(1);{int r=std::min(7,rem);dec_zeros(out,r);return r;}}
        if(!(b&2)){br.read(2);{int r=std::min(2,rem);dec_zeros(out,r);return r;}}
        if(!(b&4)){br.read(3);out[0]=0;return 1;} br.read(4);out[0]=sar(b,2)-2;return 1;
    case 3:
        b=br.peek(3); if(!(b&1)){br.read(1);{int r=std::min(3,rem);dec_zeros(out,r);return r;}}
        if(!(b&2)){br.read(2);out[0]=0;return 1;} br.read(3);out[0]=sar(b,1)-2;return 1;
    case 4:
        b=br.peek(4); if(!(b&1)){br.read(1);{int r=std::min(4,rem);dec_zeros(out,r);return r;}}
        if(!(b&2)){br.read(2);{int r=std::min(2,rem);dec_zeros(out,r);return r;}}
        if(!(b&4)){br.read(3);out[0]=0;return 1;} br.read(4);out[0]=sar(b,2)-2;return 1;
    case 5: v=br.read(2);if(v==0){int r=std::min(6,rem);dec_zeros(out,r);return r;}out[0]=v-2;return 1;
    case 6:
        b=br.peek(3);if(!(b&1)){br.read(1);out[0]=0;return 1;}
        br.read(3);v=sar(b,1);out[0]=sar(v,1)+v-2;return 1;
    case 7:
        b=br.peek(3);low2=b&3;if(low2!=0){br.read(2);out[0]=low2-2;return 1;}br.read(3);out[0]=(b&7)-2;return 1;
    case 8:
        b=br.peek(5);if(!(b&1)){br.read(1);{int r=std::min(4,rem);dec_zeros(out,r);return r;}}
        if(!(b&2)){br.read(2);{int r=std::min(2,rem);dec_zeros(out,r);return r;}}
        if(!(b&4)){br.read(3);out[0]=0;return 1;}br.read(5);v=sar(b,3);out[0]=sar(v,1)+v-2;return 1;
    case 9:
        b=br.peek(4);low2=b&3;if(low2!=0){br.read(2);out[0]=low2-2;return 1;}
        v=sar(b,2);if(!(v&2))v+=-3;br.read(4);out[0]=v;return 1;
    case 10: v=br.read(3);if(v==0){int r=std::min(3,rem);dec_zeros(out,r);return r;}out[0]=v-4;return 1;
    case 11:
        b=br.peek(5);low2=b&3;if(low2!=0){br.read(2);out[0]=low2-2;return 1;}
        v=sar(b,2);if(v&4)v+=-2;else v+=-5;br.read(5);out[0]=v;return 1;
    case 12: v=br.read(4);if(v==0){int r=std::min(4,rem);dec_zeros(out,r);return r;}out[0]=v-8;return 1;
    case 13:
        b=br.peek(5);low3=b&7;if(low3>=3){br.read(3);out[0]=low3-5;return 1;}
        if(low3==2){br.read(4);out[0]=(b&8)?3:-3;return 1;}
        if(low3&1){br.read(5);out[0]=-4-sar(b,3);return 1;}br.read(5);out[0]=sar(b,3)+4;return 1;
    case 14:
        b=br.peek(6);low2=b&3;if(low2!=0){br.read(2);out[0]=low2-2;return 1;}
        if(!(b&4)){v=sar(b,3)&3;if(!(v&2))v+=-3;br.read(5);out[0]=v;return 1;}
        v=sar(b,3);if(!(v&4))v+=-7;br.read(6);out[0]=v;return 1;
    case 15:
        b=br.peek(5);if(!(b&1)){br.read(4);v=b&0xF;if(v==0){int r=std::min(4,rem);dec_zeros(out,r);return r;}out[0]=sar(v,1)-4;return 1;}
        {int32_t c=sar(b,1)&1;int32_t a=sar(b,2);if(!(a&4))a+=-7;br.read(5);out[0]=a<<c;return 1;}
    case 16:
        b=br.peek(7);low2=b&3;if(low2!=0){br.read(2);out[0]=low2-2;return 1;}
        if(!(b&4)){v=sar(b,3)&3;if(!(v&2))v+=-3;br.read(5);out[0]=v;return 1;}
        {int32_t c=sar(b,3)&1;v=sar(b,4);if(!(v&4))v+=-7;br.read(7);out[0]=v<<c;return 1;}
    case 17:
        b=br.peek(7);if(!(b&1)){br.read(2);if(!(b&2)){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=0;return 1;}
        if(!(b&2)){br.read(3);v=sar(b,1)&2;out[0]=v-1;return 1;}
        if(!(b&4)){br.read(5);v=sar(b,3)&3;if(!(v&2))v+=-3;out[0]=v;return 1;}
        {int32_t c=sar(b,3)&1;v=sar(b,4);if(!(v&4))v+=-7;br.read(7);out[0]=v<<c;return 1;}
    case 18:
        v=br.read(5);if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}
        {int32_t l2=v&3;if(l2==0){out[0]=sar(v,2)-4;return 1;}int32_t c=l2-1;int32_t a=sar(v,2);if(!(a&4))a+=-7;out[0]=a<<c;return 1;}
    case 19:
        b=br.peek(6);if(!(b&1)){br.read(4);v=b&0xF;if(v==0){int r=std::min(4,rem);dec_zeros(out,r);return r;}out[0]=sar(v,1)-4;return 1;}
        if(!(b&2)){v=sar(b,2)&7;br.read(5);if(!(v&4))v+=-7;out[0]=v;return 1;}
        {int32_t sh=(sar(b,2)&1)+1;v=sar(b,3);br.read(6);if(!(v&4))v+=-7;out[0]=v<<sh;return 1;}
    case 20:
        b=br.peek(7);low3=b&7;if(low3>=3){br.read(3);out[0]=low3-5;return 1;}
        if(low3==0){br.read(4);out[0]=(b&8)?3:-3;return 1;}
        if(low3==1){v=sar(b,3)&7;br.read(6);if(!(v&4))v+=-7;out[0]=v;return 1;}
        {int32_t sh=(sar(b,3)&1)+1;v=sar(b,4);br.read(7);if(!(v&4))v+=-7;out[0]=v<<sh;return 1;}
    case 21:
        b=br.peek(7);if(!(b&1)){br.read(2);if(!(b&2)){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=0;return 1;}
        if(!(b&2)){br.read(3);v=sar(b,1)&2;out[0]=v-1;return 1;}
        if(!(b&0xC)){br.read(6);v=sar(b,4)&3;if(!(v&2))v+=-3;out[0]=v;return 1;}
        {int32_t c=(sar(b,2)&3)-1;v=sar(b,4);if(!(v&4))v+=-7;br.read(7);out[0]=v<<c;return 1;}
    case 22:
        b=br.peek(6);if(!(b&1)){br.read(4);v=b&0xF;if(v==0){int r=std::min(4,rem);dec_zeros(out,r);return r;}out[0]=sar(v,1)-4;return 1;}
        {int32_t c=sar(b,1)&3;v=sar(b,3);if(!(v&4))v+=-7;br.read(6);out[0]=v<<c;return 1;}
    case 23:
        b=br.peek(6);if(!(b&1)){br.read(5);v=b&0x1F;if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}out[0]=sar(v,1)-8;return 1;}
        if(!(b&2)){v=sar(b,2)&7;br.read(5);if(!(v&4))v+=-7;out[0]=v<<1;return 1;}
        {int32_t sh=(sar(b,2)&1)+2;v=sar(b,3);br.read(6);if(!(v&4))v+=-7;out[0]=v<<sh;return 1;}
    case 24:
        b=br.peek(6);if(!(b&1)){br.read(5);v=b&0x1F;if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}out[0]=sar(v,1)-8;return 1;}
        {int32_t c=(sar(b,1)&3)+1;v=sar(b,3);if(!(v&4))v+=-7;br.read(6);out[0]=v<<c;return 1;}
    case 25:
        b=br.peek(6);low3=b&7;if(low3<2){br.read(5);v=b&0x1F;if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}out[0]=(v&1)?sar(v,3):-sar(v,3);return 1;}
        v=sar(b,3);if(!(v&4))v+=-7;br.read(6);out[0]=v<<(low3-2);return 1;
    case 26:
        b=br.peek(7);if(!(b&1)){br.read(5);v=b&0x1F;if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}out[0]=sar(v,1)-8;return 1;}
        {int32_t c=(sar(b,1)&7)+1;v=sar(b,4);if(!(v&4))v+=-7;br.read(7);out[0]=v<<c;return 1;}
    case 27:
        b=br.peek(9);low3=b&7;if(low3<2){br.read(5);v=b&0x1F;if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}out[0]=(v&1)?sar(v,3):-sar(v,3);return 1;}
        if(low3!=7){v=sar(b,3)&7;br.read(6);if(!(v&4))v+=-7;out[0]=v<<(low3-2);return 1;}
        {int32_t sh=(sar(b,3)&7)+5;v=sar(b,6);br.read(9);if(!(v&4))v+=-7;out[0]=v<<sh;return 1;}
    case 28:
        b=br.peek(10);low3=b&7;if(low3<2){br.read(5);v=b&0x1F;if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}out[0]=(v&1)?sar(v,3):-sar(v,3);return 1;}
        if(low3!=7){v=sar(b,3)&7;br.read(6);if(!(v&4))v+=-7;out[0]=v<<(low3-2);return 1;}
        {int32_t sh=(sar(b,3)&0xF)+5;v=sar(b,7);br.read(10);if(!(v&4))v+=-7;out[0]=v<<sh;return 1;}
    case 29:{
        b=br.peek(8);int32_t low5=b&0x1F;if(low5==2){br.read(5);out[0]=0;return 1;}
        if(low5<2){if(b&1)v=(sar(b,5)&3)+1;else v=-1-(sar(b,5)&3);br.read(7);out[0]=v;return 1;}
        v=sar(b,5);if(!(v&4))v+=-7;br.read(8);out[0]=v<<(low5-3);return 1;}
    case 45: v=br.read(5);if(v==0){int r=std::min(5,rem);dec_zeros(out,r);return r;}out[0]=v-16;return 1;
    case 46:
        b=br.peek(7);if(!(b&1)){br.read(1);{int r=std::min(4,rem);dec_zeros(out,r);return r;}}
        if(!(b&2)){br.read(2);out[0]=0;return 1;}br.read(7);v=sar(b,2);out[0]=sar(v,4)+v-0x10;return 1;
    case 47:
        b=br.peek(7);if(!(b&1)){br.read(2);if(!(b&2)){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=0;return 1;}
        if(!(b&2)){br.read(3);v=sar(b,1)&2;out[0]=v-1;return 1;}
        br.read(7);v=sar(b,2);out[0]=(v&0x10)?v-14:v-17;return 1;
    case 48:
        b=br.peek(7);low2=b&3;if(low2!=0){br.read(2);out[0]=low2-2;return 1;}
        br.read(7);v=sar(b,2);out[0]=(v&0x10)?v-14:v-17;return 1;
    case 49: v=br.read(6);if(v==0){int r=std::min(6,rem);dec_zeros(out,r);return r;}out[0]=v-32;return 1;
    case 50:
        b=br.peek(8);if(!(b&1)){br.read(2);if(!(b&2)){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=0;return 1;}
        if(!(b&2)){br.read(3);v=sar(b,1)&2;out[0]=v-1;return 1;}
        br.read(8);v=sar(b,2);out[0]=(v&0x20)?v-30:v-33;return 1;
    case 51:
        b=br.peek(8);low3=b&7;if(low3>=3){br.read(3);out[0]=low3-5;return 1;}
        if(low3==2){br.read(4);out[0]=(b&8)?3:-3;return 1;}
        if(low3&1){br.read(8);out[0]=sar(b,3)+4;return 1;}br.read(8);out[0]=-4-sar(b,3);return 1;
    case 52: v=br.read(7);if(v==0){int r=std::min(7,rem);dec_zeros(out,r);return r;}out[0]=v-64;return 1;
    case 53: v=br.read(8);if(v==0){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=v-128;return 1;
    case 54: v=br.read(9);if(v==0){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=v-256;return 1;
    case 55: v=br.read(10);if(v==0){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=v-512;return 1;
    case 56: v=br.read(11);if(v==0){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=v-1024;return 1;
    case 57: v=br.read(16);if(v==0){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=v-0x8000;return 1;
    case 58: v=br.read(24);if(v==0){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=v-0x800000;return 1;
    case 59:{uint32_t u=br.read(32);if(u==0x80000000u){int r=std::min(8,rem);dec_zeros(out,r);return r;}out[0]=(int32_t)u;return 1;}
    default: out[0]=0; return 1;
    }
}

static void decode_channel(BitReader& br, int n, int codec, int32_t* out) {
    // IDA funcs_32BF4F dispatch:
    //   0,30,31,62,63 -> zero fill
    //   32..46        -> duplicate of 0..14 for negative-scale channels
    //   47..61        -> wide-value implementations kept here as cases 45..59
    if (codec == 0 || codec == 30 || codec == 31 || codec >= 62) {
        dec_zeros(out, n);
        return;
    }
    if (codec >= 32 && codec <= 46) {
        decode_channel(br, n, codec - 32, out);
        return;
    }

    int impl_codec = (codec >= 47 && codec <= 61) ? codec - 2 : codec;
    int pos=0; while(pos<n) pos+=dec_one(br,n-pos,impl_codec,out+pos);
}

static glm::quat qmul(const glm::quat& a, const glm::quat& b) {
    return glm::quat(
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
    );
}

static int32_t decode_zigzag(uint32_t v) {
    return (v & 1u) ? -(int32_t)(v >> 1) : (int32_t)(v >> 1);
}

static void skip_bits(BitReader& br, size_t n) {
    size_t bit_pos = br.tell() + n;
    br.bp = std::min(br.len, bit_pos / 8);
    br.bi = (br.bp < br.len) ? (int)(bit_pos % 8) : 0;
}

static int32_t read_selector(BitReader& br) {
    static const int widths[4] = {3, 5, 8, 21};
    uint32_t mode = br.read(2);
    return decode_zigzag(br.read(widths[mode & 3u]));
}

static int32_t read_entropy_quat_seed(BitReader& br) {
    uint32_t code = br.peek(12);
    if ((code & 0x1Fu) == 0) {
        br.read(12);
        int32_t base = (int32_t)(code >> 9);
        if ((base & 4) == 0) base -= 7;
        int shift = (int)((code >> 5) & 0x0F) + 9;
        return base * (1 << shift);
    }

    if (code & 1u) {
        br.read(7);
        int32_t base = (int32_t)((code >> 4) & 7);
        if ((base & 4) == 0) base -= 7;
        int shift = (int)((code >> 1) & 7) + 1;
        return base * (1 << shift);
    }

    br.read(5);
    return (int32_t)((code >> 1) & 0x0F) - 8;
}

static glm::quat quat_from_scaled_vec(float x, float y, float z) {
    float sq = x*x + y*y + z*z;
    float w = std::sqrt(std::abs(1.0f - sq));
    return glm::quat(w, x, y, z);
}

static glm::quat game_quat_to_glm(const glm::quat& q) {
    return glm::conjugate(q);
}

static glm::quat unpack_packed_quat3(const glm::quat& q) {
    glm::quat src = q;
    if (src.w < 0.0f) src = -src;

    int8_t sx = (int8_t)(int)(src.x * 127.0f);
    int8_t sy = (int8_t)(int)(src.y * 127.0f);
    int8_t sz = (int8_t)(int)(src.z * 127.0f);

    float x = (float)sx * (1.0f / 127.0f);
    float y = (float)sy * (1.0f / 127.0f);
    float z = (float)sz * (1.0f / 127.0f);
    return quat_from_scaled_vec(x, y, z);
}

static glm::quat normalized_or_identity(const glm::quat& q) {
    float m2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    if (!std::isfinite(m2) || m2 <= 1e-12f) return glm::quat(1, 0, 0, 0);
    return q * (1.0f / std::sqrt(m2));
}

struct EntropyQuatPacketState {
    glm::quat q = glm::quat(1, 0, 0, 0);
    int32_t accum[3] = {0, 0, 0};
    BitReader channels[3] = {
        BitReader(nullptr, 0),
        BitReader(nullptr, 0),
        BitReader(nullptr, 0)
    };
    uint8_t codecs[3] = {0, 0, 0};
};

static bool init_entropy_quat_packet(const std::vector<uint8_t>& packet,
                                     int frame_count,
                                     float scale,
                                     EntropyQuatPacketState& st) {
    if (packet.empty()) return false;

    float abs_scale = std::abs(scale);
    float initial[3] = {0.f, 0.f, 0.f};
    BitReader next_axis(packet.data(), packet.size());

    for (int axis = 0; axis < 3; ++axis) {
        BitReader br = next_axis;
        uint32_t next_axis_bit_delta = 0;

        if (axis < 2 && br.read(1)) {
            next_axis_bit_delta = br.read(11) + 256;
        }

        uint8_t codec = (uint8_t)br.read(5);
        if (scale < 0.0f) codec = (uint8_t)(codec + 32);
        st.codecs[axis] = codec;

        int32_t first = read_selector(br);
        initial[axis] = (float)first * abs_scale * 0.25f;

        if (scale < 0.0f)
            st.accum[axis] = read_selector(br);
        else
            st.accum[axis] = read_entropy_quat_seed(br);

        st.channels[axis] = br;

        if (axis < 2) {
            if (next_axis_bit_delta) {
                next_axis = br;
                skip_bits(next_axis, next_axis_bit_delta);
            } else {
                next_axis = br;
                int skip_count = std::max(0, frame_count - 2);
                if (skip_count > 0) {
                    std::vector<int32_t> discard(skip_count);
                    decode_channel(next_axis, skip_count, codec, discard.data());
                }
            }
        }
    }

    st.q = quat_from_scaled_vec(initial[0], initial[1], initial[2]);
    return true;
}

static std::vector<glm::quat> decode_entropy_quat_packet(const std::vector<uint8_t>& packet,
                                                         int frame_count,
                                                         float scale,
                                                         bool packed_output = false) {
    std::vector<glm::quat> out(std::max(0, frame_count), glm::quat(1, 0, 0, 0));
    if (frame_count <= 0) return out;

    EntropyQuatPacketState st;
    if (!init_entropy_quat_packet(packet, frame_count, scale, st)) return out;

    glm::quat cur = st.q;
    out[0] = packed_output ? unpack_packed_quat3(cur) : cur;

    // Seed-only / constant track. Packets too short to carry per-frame entropy
    // delta channels (the 4-byte tracks — e.g. ARMORED_THUG hit-reaction
    // r-forearm/hand/finger, bytes "00 00 20 04") are constant: the bone holds
    // one local orientation for the whole clip. Verified via IDA
    // (NAL_WritePackedQuaternionTrack_DeltaAccum @0x332f70): the warm-up flags at
    // state+0x15 make the writer emit the SEED before entropy-delta accumulation
    // begins, and a seed-only source never reaches the accumulate loop. Running
    // the second-order integrator below on such a packet re-applies the seed
    // velocity every frame and spins the bone away (model collapse); skipping the
    // packet drops the seed and freezes the bone at rest (arms in wrong pose).
    // Correct behavior: hold the decoded seed for all frames. Threshold <5 so it
    // touches ONLY the 4-byte constants — every working track (>=6 bytes,
    // including the 6-byte rest-seed clavicles) decodes exactly as before.
    if ((int)packet.size() < 5) {
        for (int f = 1; f < frame_count; ++f) out[f] = out[0];
        return out;
    }

    float abs_scale = std::abs(scale);
    auto apply_accum_delta = [&]() {
        glm::quat delta = quat_from_scaled_vec((float)st.accum[0] * abs_scale,
                                               (float)st.accum[1] * abs_scale,
                                               (float)st.accum[2] * abs_scale);
        cur = packed_output ? qmul(delta, cur)
                            : normalized_or_identity(qmul(delta, cur));
        return packed_output ? unpack_packed_quat3(cur) : cur;
    };

    if (frame_count >= 2) {
        out[1] = apply_accum_delta();
    }

    int remaining = frame_count - 2;
    if (remaining > 0) {
        std::vector<int32_t> dx(remaining), dy(remaining), dz(remaining);
        decode_channel(st.channels[0], remaining, st.codecs[0], dx.data());
        decode_channel(st.channels[1], remaining, st.codecs[1], dy.data());
        decode_channel(st.channels[2], remaining, st.codecs[2], dz.data());

        for (int i = 0; i < remaining; ++i) {
            st.accum[0] += dx[i];
            st.accum[1] += dy[i];
            st.accum[2] += dz[i];
            out[i + 2] = apply_accum_delta();
        }
    }

    return out;
}

static int32_t read_entropy_float_seed(BitReader& br) {
    if (br.read(1)) {
        return (int32_t)br.read(32);
    }
    return (int32_t)br.read(16) - 0x8000;
}

static int32_t read_entropy_float_delta_seed(BitReader& br) {
    uint32_t code = br.peek(12);
    if ((code & 0x1Fu) == 0) {
        br.read(12);
        int32_t base = (int32_t)(code >> 9);
        if ((base & 4) == 0) base -= 7;
        int shift = (int)((code >> 5) & 0x0F) + 17;
        return base * (1 << shift);
    }

    if (code & 1u) {
        br.read(8);
        int32_t base = (int32_t)((code >> 5) & 7);
        if ((base & 4) == 0) base -= 7;
        int shift = (int)((code >> 1) & 0x0F) + 1;
        return base * (1 << shift);
    }

    br.read(5);
    return (int32_t)((code >> 1) & 0x0F) - 8;
}

static std::vector<float> decode_entropy_float_packet(const std::vector<uint8_t>& packet,
                                                      int frame_count,
                                                      float scale) {
    std::vector<float> out(std::max(0, frame_count), 0.0f);
    if (frame_count <= 0 || packet.empty()) return out;

    BitReader br(packet.data(), packet.size());
    uint8_t codec = (uint8_t)br.read(5);
    float abs_scale = std::abs(scale);
    if (scale < 0.0f) codec = (uint8_t)(codec + 32);

    int32_t prev_prev = read_entropy_float_seed(br);
    out[0] = (float)prev_prev * abs_scale;
    if (frame_count == 1) return out;

    int32_t seed_delta = (codec & 0x20)
                       ? read_entropy_float_seed(br)
                       : read_entropy_float_delta_seed(br);
    int32_t prev = prev_prev + seed_delta;
    out[1] = (float)prev * abs_scale;

    int remaining = frame_count - 2;
    if (remaining > 0) {
        std::vector<int32_t> deltas(remaining);
        decode_channel(br, remaining, codec, deltas.data());
        for (int i = 0; i < remaining; ++i) {
            int32_t cur = deltas[i] + 2 * prev - prev_prev;
            out[i + 2] = (float)cur * abs_scale;
            prev_prev = prev;
            prev = cur;
        }
    }

    return out;
}

// ============================================================
// Decode all frames from NAL entropy quaternion packets.
// ============================================================
void AnimClip::decode_all_frames() const {
    if (frames_decoded) return;
    frames_decoded = true;
    cached_frames.assign(frame_count, std::vector<BonePose>(n_bones));
    cached_root_orientations.assign(frame_count, glm::quat(1, 0, 0, 0));

    // Fill skeleton tracks with rest pose until a packet overrides them.
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
            // Skip ONLY truly-empty (0-byte) tracks: those carry no data, so
            // decoding them yields identity and would OVERWRITE the bone's rest
            // pose with identity (this is what exploded the legs). Non-empty short
            // packets are seed-only constants and are handled inside
            // decode_entropy_quat_packet (it holds the seed instead of
            // integrating) — see the note there. Verified via IDA.
            if (nf >= 2 && sec.quat_packets[qi].empty()) continue;
            float track_scale = (qi < (int)sec.quat_scales.size()) ? sec.quat_scales[qi] : 0.001f;
            // packed_output=false: decode at full float precision. The game packs
            // these to 3 signed bytes (~1/127 ~= 0.8deg steps) for storage, but for
            // the viewer that quantization stair-steps slow motion (idle sway lands
            // one byte-level per frame) and reads as jitter under interpolation.
            // The entropy delta stream is finer than the byte-pack, so decoding
            // unpacked is smoother and within <1deg of the game's reconstruction.
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

// ============================================================
// File parsing
// ============================================================
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

        // BC stream order: source 0 ae_base_bone PO for bip01 pelvis, then
        // 59 nal_quaternion packets whose source ids target bones 1..59.
        for (int axis = 0; axis < 3; ++axis)
            read_packet(sec.root_pos_packets[axis]);
        read_packet(sec.root_quat_packet);

        // Quaternion-track count: the skeleton's source-descriptor list names
        // exactly one "nal_quaternion" per animated rotation track, so that count
        // is authoritative. The old bone_count-1 over-counts by the trailing
        // fakeroot (e.g. MINION_LIZARD: 44 sources vs bone_count-1 = 45), which
        // read one packet too many and desynced the stream tail. Fall back to
        // bone_count-1 / BC default only when the source list isn't found.
        // (Black Cat is 59 either way, so working rigs are unaffected.)
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
            sec.quat_scales.push_back(s);
        }

        // The remaining BC sources are ae_floor_offset, nal_trajectory,
        // spidey_signal, then nal_position triples. Keep the cursor faithful
        // to the game source descriptors, but do not apply position tracks
        // until their per-axis skeleton metadata is fully modeled.
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

        // Any .dat with the NAL animation magic is a clip, regardless of name.
        // Skeleton .dat use magic 0x00B5B58C and mesh/data .dat use 0x7BAD*, so
        // the magic check alone separates animations from everything else.
        // Read the header through the VFS (serves from a mounted .iso or real FS).
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
