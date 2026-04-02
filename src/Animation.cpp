#include "Animation.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <filesystem>

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}

// Per-bone skeleton scale factors for Black Cat (from BLACK_CAT.dat offset 0x2AE0)
static const int BC_PALETTE[24] = {
    0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 29, 30, 31, 32, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59
};
static const float SKEL_BONE_SCALE[60] = {
     0.00100000f, 0.00100000f, 0.00100000f, 0.00100000f, 0.00100000f, 0.00100000f,
     0.00781250f, 0.00781250f, 0.00781250f, 0.00100000f, 0.00100000f, 0.00781250f,
     0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
     0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
     0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.00100000f, 0.00100000f,
     0.00100000f, 0.00100000f, 0.00781250f, 0.01562500f, 0.01562500f, 0.01562500f,
     0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
     0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f, 0.01562500f,
     0.01562500f, 0.00100000f, 0.00100000f, 0.00100000f, 0.00100000f, 0.00100000f,
     0.00195312f, 0.00195312f, 0.00195312f, 0.00100000f, 0.00195312f, 0.00195312f,
};

glm::mat4 BonePose::to_matrix() const { return glm::mat4_cast(q); }

// ============================================================
// Rest pose loader
// ============================================================
std::vector<glm::quat> load_skeleton_rest_pose(const std::string& skel_path) {
    auto data = read_file(skel_path);
    constexpr size_t REST_OFFSET = 0x2400;
    constexpr int    MAX_BONES   = 60;
    constexpr int    STRIDE      = 16;

    std::vector<glm::quat> out;
    if (data.size() < REST_OFFSET + MAX_BONES * STRIDE) return out;

    out.resize(MAX_BONES);
    for (int i = 0; i < MAX_BONES; i++) {
        size_t off = REST_OFFSET + i * STRIDE;
        float x = rd<float>(data.data(), off);
        float y = rd<float>(data.data(), off + 4);
        float z = rd<float>(data.data(), off + 8);
        float w = rd<float>(data.data(), off + 12);
        out[i] = glm::quat(w, x, y, z);
    }
    return out;
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
        b=br.peek(3);low2=b&3;if(low2!=0){br.read(2);out[0]=low2-2;return 1;}br.read(3);out[0]=-2;return 1;
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
        b=br.peek(5);low3=b&7;if(low3>=3){br.read(3);out[0]=low3-3;return 1;}
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
        b=br.peek(7);low3=b&7;if(low3>=3){br.read(3);out[0]=low3-3;return 1;}
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
        b=br.peek(8);low3=b&7;if(low3>=3){br.read(3);out[0]=low3-3;return 1;}
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
    if (codec==0||codec==30||codec==31||codec==60||codec==61||codec==62||codec==63){dec_zeros(out,n);return;}
    if (codec>=32&&codec<=44){decode_channel(br,n,codec-32,out);return;}
    int pos=0; while(pos<n) pos+=dec_one(br,n-pos,codec,out+pos);
}

static glm::quat qmul(const glm::quat& a, const glm::quat& b) {
    return glm::quat(
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
    );
}

// ============================================================
// Decode all frames — Hamilton multiply chain from rest pose
// ============================================================
void AnimClip::decode_all_frames() const {
    if (frames_decoded) return;
    frames_decoded = true;
    cached_frames.assign(frame_count, std::vector<BonePose>(n_bones));

    // Fill ALL bones with rest pose (inactive bones keep this)
    for (int f = 0; f < frame_count; f++)
        for (int bi = 0; bi < n_bones; bi++)
            if (bi < (int)rest_pose.size())
                cached_frames[f][bi].q = rest_pose[bi];

    for (auto& sec : sections) {
        if (sec.frame_count <= 0 || sec.n_active <= 0) continue;

        BitReader br(sec.bitstream.data(), sec.bitstream.size(),
                     sec.bitstream_bit_offset / 8, sec.bitstream_bit_offset % 8);

        int nf = sec.frame_count;
        int na = sec.n_active;

        for (int bi = 0; bi < na; bi++) {
            int cx = sec.codec_x[bi], cy = sec.codec_y[bi], cz = sec.codec_z[bi];
            int32_t dx[512], dy[512], dz[512];
            decode_channel(br, nf, cx, dx);
            decode_channel(br, nf, cy, dy);
            decode_channel(br, nf, cz, dz);

            int skel_idx = (bi < 24) ? BC_PALETTE[bi] : bi;
            float skel_f = (skel_idx < 60) ? SKEL_BONE_SCALE[skel_idx] : 0.001f;
            float eff_scale = std::abs(skel_f) * qscale;

            // Hamilton chain starts from rest pose — output is absolute NAL quaternion
            glm::quat prev(1, 0, 0, 0);
            if (bi < (int)rest_pose.size())
                prev = rest_pose[bi];

            int32_t sx = 0, sy = 0, sz = 0;
            for (int f = 0; f < nf; f++) {
                sx += dx[f]; sy += dy[f]; sz += dz[f];
                float fx = (float)sx * eff_scale;
                float fy = (float)sy * eff_scale;
                float fz = (float)sz * eff_scale;

                float sq = fx*fx + fy*fy + fz*fz;
                float fw = (sq < 1.0f) ? std::sqrt(1.0f - sq) : 0.0f;
                glm::quat cur(fw, fx, fy, fz);

                prev = qmul(cur, prev);

                int abs_f = sec.frame_start + f;
                if (abs_f < frame_count && bi < n_bones)
                    cached_frames[abs_f][bi].q = prev;
            }
        }
    }
}

std::vector<BonePose> AnimClip::sample_pose(float t) const {
    if (!loaded || sections.empty()) return std::vector<BonePose>(n_bones);
    decode_all_frames();
    float clamped = looping ? std::fmod(t, duration) : std::clamp(t, 0.f, duration);
    if (clamped < 0) clamped += duration;
    int frame = std::clamp((int)(clamped * fps), 0, frame_count - 1);
    if (frame < (int)cached_frames.size()) return cached_frames[frame];
    return std::vector<BonePose>(n_bones);
}

// ============================================================
// File parsing
// ============================================================
void parse_animation(AnimClip& clip) {
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
    clip.qscale = rd<float>(d, 0x100);

    size_t sec_table = 0x100 + ref_size;
    if (sec_table + sec_count * 4 > sz) { clip.loaded = false; return; }

    std::vector<uint32_t> sec_offsets(sec_count);
    for (uint32_t i = 0; i < sec_count; i++)
        sec_offsets[i] = rd<uint32_t>(d, sec_table + i * 4);

    size_t sec_data_base = sec_table + sec_count * 4;

    clip.n_bones     = 24;
    clip.track_count = 72;
    clip.sections.resize(sec_count);
    clip.frames_decoded = false;
    clip.cached_frames.clear();

    for (uint32_t si = 0; si < sec_count; si++) {
        size_t sec_start = sec_data_base + sec_offsets[si];
        size_t sec_end = (si + 1 < sec_count) ? sec_data_base + sec_offsets[si + 1] : sz;
        if (sec_start >= sz) continue;
        int frames_in_sec = (int)std::min((uint32_t)(clip.frame_count - si * max_fps), max_fps);
        if (frames_in_sec <= 0) continue;

        auto& sec = clip.sections[si];
        sec.frame_start = (int)(si * max_fps);
        sec.frame_count = frames_in_sec;
        sec.n_active = d[sec_start];
        if (sec.n_active > clip.n_bones) sec.n_active = clip.n_bones;

        sec.codec_x.resize(sec.n_active);
        sec.codec_y.resize(sec.n_active);
        sec.codec_z.resize(sec.n_active);

        BitReader hdr(d, sz, sec_start + 1);
        for (int bi = 0; bi < sec.n_active; bi++) {
            sec.codec_x[bi] = (uint8_t)hdr.read(6);
            sec.codec_y[bi] = (uint8_t)hdr.read(6);
            sec.codec_z[bi] = (uint8_t)hdr.read(6);
        }

        size_t bit_off = hdr.tell() - sec_start * 8;
        sec.bitstream.assign(d + sec_start, d + std::min(sec_end, sz));
        sec.bitstream_bit_offset = bit_off;
    }

    clip.loaded = true;
    int max_active = 0;
    for (auto& s : clip.sections) max_active = std::max(max_active, s.n_active);
    std::cout << "[ANIM] '" << clip.name << "': "
              << clip.frame_count << "F " << sec_count << "sec "
              << max_active << "/" << clip.n_bones << " bones "
              << "qs=" << clip.qscale << " "
              << (clip.looping ? "loop" : "once") << "\n";
}

std::vector<AnimClip> scan_animations(const std::string& folder) {
    std::vector<AnimClip> clips;
    namespace fs = std::filesystem;
    if (!fs::is_directory(folder)) return clips;

    for (auto& entry : fs::directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        std::string path = entry.path().string();
        std::string stem = entry.path().stem().string();
        std::string ext  = entry.path().extension().string();
        for (auto& c : ext) c = tolower(c);
        if (ext != ".dat") continue;
        if (stem.size() < 3) continue;
        std::string upper = stem;
        for (auto& c : upper) c = toupper(c);
        if (upper.substr(0, 2) != "BC") continue;
        if (upper.find("BLACK_CAT") != std::string::npos) continue;

        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        uint32_t ver = 0;
        f.read(reinterpret_cast<char*>(&ver), 4);
        if (ver != 0x00010101) continue;

        AnimClip clip;
        clip.name = stem; clip.path = path;
        f.seekg(0xA4); uint32_t lf=0; f.read((char*)&lf,4); clip.looping=lf!=0;
        f.seekg(0xA8); float dur=0; f.read((char*)&dur,4); clip.duration=dur;
        f.seekg(0xB0); float fp=0;  f.read((char*)&fp,4);  clip.fps=fp;
        f.seekg(0xB4); uint32_t fc=0; f.read((char*)&fc,4); clip.frame_count=fc;
        clip.n_bones=24; clip.track_count=72;
        clips.push_back(std::move(clip));
    }
    std::sort(clips.begin(), clips.end(),
              [](const AnimClip& a, const AnimClip& b) { return a.name < b.name; });
    return clips;
}