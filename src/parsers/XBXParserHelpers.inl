static std::vector<uint8_t> read_file(const std::string& path) {
    return vfs::read_file(path);   
}

template<typename T>
static T rd(const uint8_t* p, size_t off) {
    T v; memcpy(&v, p + off, sizeof(T)); return v;
}

static std::string read_mat_name(const uint8_t* data, size_t ptr) {
    const char* s = reinterpret_cast<const char*>(data + ptr + 4);
    return std::string(s, strnlen(s, 28));
}



static bool xbx_is_affine_mat(const uint8_t* d, size_t sz, size_t o) {
    if (o + 64 > sz) return false;
    if (std::fabs(rd<float>(d, o + 12)) > 1e-3f) return false; 
    if (std::fabs(rd<float>(d, o + 28)) > 1e-3f) return false; 
    if (std::fabs(rd<float>(d, o + 44)) > 1e-3f) return false; 
    if (std::fabs(rd<float>(d, o + 60) - 1.0f) > 1e-3f) return false; 
    for (int c = 0; c < 3; ++c) {
        float a = rd<float>(d, o + c * 16);
        float b = rd<float>(d, o + c * 16 + 4);
        float e = rd<float>(d, o + c * 16 + 8);
        float m = a * a + b * b + e * e;
        if (!(m > 0.25f && m < 4.0f)) return false; 
    }
    return true;
}







size_t xbx_find_bind_matrix_base(const uint8_t* d, size_t sz, int* out_count) {
    auto count_run = [&](size_t o) {
        int n = 0; while (xbx_is_affine_mat(d, sz, o + (size_t)n * 64)) ++n; return n;
    };
    size_t best = 0; int best_n = 0;
    for (size_t o = 0x40; o + 64 <= sz; o += 4) {
        if (!xbx_is_affine_mat(d, sz, o)) continue;
        float tx = rd<float>(d, o + 48), ty = rd<float>(d, o + 52), tz = rd<float>(d, o + 56);
        if (tx * tx + ty * ty + tz * tz > 4e-4f) continue; 
        int n = count_run(o);
        if (n >= 4) { best = o; best_n = n; break; }       
    }
    
    if (!best) {
        for (size_t o = 0x40; o + 64 <= sz; o += 4) {
            if (!xbx_is_affine_mat(d, sz, o)) continue;
            int n = count_run(o);
            if (n >= 8) { best = o; best_n = n; break; }
        }
    }
    if (!best) { best = 0x2f0; best_n = 0; }
    if (out_count) *out_count = best_n;
    return best;
}




static std::vector<uint32_t> strip_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    int parity = 0;
    for (size_t i = 0; i + 2 < n; ++i) {
        uint32_t a=idx[i], b=idx[i+1], c=idx[i+2];
        if (a >= vc || b >= vc || c >= vc) { parity ^= 1; continue; }
        if (a==b || b==c || a==c)          { parity ^= 1; continue; }
        if (parity) { tris.push_back(a); tris.push_back(c); tris.push_back(b); }
        else        { tris.push_back(a); tris.push_back(b); tris.push_back(c); }
        parity ^= 1;
    }
    return tris;
}


static std::vector<uint32_t> trilist_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    for (size_t i = 0; i + 2 < n; i += 3) {
        uint32_t a=idx[i], b=idx[i+1], c=idx[i+2];
        if (a >= vc || b >= vc || c >= vc) continue;
        if (a==b || b==c || a==c) continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
    }
    return tris;
}


static std::vector<uint32_t> quadlist_to_list(const uint16_t* idx, size_t n, uint32_t vc) {
    std::vector<uint32_t> tris;
    for (size_t i = 0; i + 3 < n; i += 4) {
        uint32_t a=idx[i], b=idx[i+1], c=idx[i+2], dd=idx[i+3];
        if (a>=vc||b>=vc||c>=vc||dd>=vc) continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
        tris.push_back(a); tris.push_back(c); tris.push_back(dd);
    }
    return tris;
}















static std::vector<uint32_t> pushbuffer_to_list(const uint8_t* d, size_t byte_len, uint32_t vc) {
    std::vector<uint16_t> indices;
    int prim = 6;                                   
    size_t o = 0;
    while (o + 4 <= byte_len) {
        uint32_t hdr    = rd<uint32_t>(d, o); o += 4;
        uint32_t method = hdr & 0x3FFFF;
        uint32_t count  = (hdr >> 18) & 0x7FF;      
        if (count == 0) continue;                   
        if (o + (size_t)count * 4 > byte_len) break;
        if (method == 0x17FC) {                     
            uint32_t p = rd<uint32_t>(d, o) & 0xFFFF;
            if (p != 0) prim = (int)p;              
        } else if (method == 0x1800) {              
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t dw = rd<uint32_t>(d, o + (size_t)k * 4);
                indices.push_back((uint16_t)(dw & 0xFFFF));
                indices.push_back((uint16_t)(dw >> 16));
            }
        }
        o += (size_t)count * 4;
    }
    if (indices.empty()) return {};
    const uint16_t* idx = indices.data();
    size_t n = indices.size();
    if (prim == 5)      return trilist_to_list(idx, n, vc);
    else if (prim == 8) return quadlist_to_list(idx, n, vc);
    else                return strip_to_list(idx, n, vc);
}
