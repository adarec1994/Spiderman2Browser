static inline int32_t sar(int32_t v, int s) { return v >> s; }
static void dec_zeros(int32_t* o, int n) { for (int i = 0; i < n; i++) o[i] = 0; }




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
