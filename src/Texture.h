#pragma once
#include <string>

// Load a DDS (DXT1/DXT5) or any stb_image-supported texture.
// Returns OpenGL texture ID, or 0 on failure.
unsigned int load_texture(const std::string& path);

// Search for hint.dds / HINT.DDS / HINT.dds in model_dir and parent.
// Returns GL texture ID or 0.
unsigned int find_texture(const std::string& hint, const std::string& model_dir);
