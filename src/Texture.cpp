#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "Texture.h"

#include <glad/glad.h>
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <iostream>
#include <cstdint>

namespace fs = std::filesystem;

static std::unordered_map<std::string, unsigned int> g_cache;

// ── DDS loader ────────────────────────────────────────────
struct DDSPixelFormat {
    uint32_t size, flags, fourCC, rgbBitCount;
    uint32_t rMask, gMask, bMask, aMask;
};
struct DDSHeader {
    uint32_t size, flags, height, width, pitchOrLinearSize;
    uint32_t depth, mipMapCount;
    uint32_t reserved[11];
    DDSPixelFormat pf;
    uint32_t caps[4], reserved2;
};

static unsigned int load_dds(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;

    uint32_t magic; f.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != 0x20534444) return 0; // "DDS "

    DDSHeader hdr;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));

    uint32_t w = hdr.width, h = hdr.height;
    uint32_t mips = std::max(1u, hdr.mipMapCount);

    // Determine format
    uint32_t gl_fmt = 0;
    uint32_t block  = 16;
    char cc[5] = {};
    memcpy(cc, &hdr.pf.fourCC, 4);

    if      (memcmp(cc,"DXT1",4)==0) { gl_fmt = 0x83F1; block = 8; }  // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
    else if (memcmp(cc,"DXT3",4)==0) { gl_fmt = 0x83F2; }             // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
    else if (memcmp(cc,"DXT5",4)==0) { gl_fmt = 0x83F3; }             // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
    else return 0;

    // Read all mip data
    size_t data_size = 0;
    uint32_t ww=w, hh=h;
    for (uint32_t m=0; m<mips; ++m) {
        data_size += std::max(1u,(ww+3)/4) * std::max(1u,(hh+3)/4) * block;
        ww=std::max(1u,ww/2); hh=std::max(1u,hh/2);
    }
    std::vector<uint8_t> pixels(data_size);
    f.read(reinterpret_cast<char*>(pixels.data()), data_size);

    unsigned int tid;
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips>1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    size_t offset = 0;
    ww=w; hh=h;
    for (uint32_t m=0; m<mips; ++m) {
        uint32_t sz = std::max(1u,(ww+3)/4) * std::max(1u,(hh+3)/4) * block;
        glCompressedTexImage2D(GL_TEXTURE_2D, m, gl_fmt, ww, hh, 0, sz, pixels.data()+offset);
        offset += sz;
        ww=std::max(1u,ww/2); hh=std::max(1u,hh/2);
    }
    if (mips == 1) glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tid;
}

// ── stb fallback ──────────────────────────────────────────
static unsigned int load_stb(const std::string& path) {
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) return 0;
    unsigned int tid;
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    return tid;
}

unsigned int load_texture(const std::string& path) {
    auto it = g_cache.find(path);
    if (it != g_cache.end()) return it->second;

    unsigned int tid = 0;
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".dds") tid = load_dds(path);
    if (!tid)          tid = load_stb(path);

    if (!tid) std::cerr << "Texture load failed: " << path << "\n";
    g_cache[path] = tid;
    return tid;
}

unsigned int find_texture(const std::string& hint, const std::string& model_dir) {
    if (hint.empty()) return 0;

    std::vector<std::string> variants = {
        hint, hint + ".dds",
        [&]{ std::string s=hint; std::transform(s.begin(),s.end(),s.begin(),::toupper); return s+".dds"; }(),
        [&]{ std::string s=hint; std::transform(s.begin(),s.end(),s.begin(),::tolower); return s+".dds"; }(),
        [&]{ std::string s=hint; std::transform(s.begin(),s.end(),s.begin(),::toupper); return s+".DDS"; }(),
    };

    std::vector<std::string> search_dirs = {
        model_dir,
        fs::path(model_dir).parent_path().string(),
        (fs::path(model_dir) / "textures").string(),
    };

    for (auto& dir : search_dirs) {
        if (!fs::is_directory(dir)) continue;
        // Build lower-case filename map
        std::unordered_map<std::string, std::string> lmap;
        for (auto& entry : fs::directory_iterator(dir)) {
            std::string fn = entry.path().filename().string();
            std::string fnl = fn;
            std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
            lmap[fnl] = (fs::path(dir) / fn).string();
        }
        for (auto& v : variants) {
            std::string vl = v;
            std::transform(vl.begin(), vl.end(), vl.begin(), ::tolower);
            auto it = lmap.find(vl);
            if (it != lmap.end()) return load_texture(it->second);
        }
    }
    return 0;
}
