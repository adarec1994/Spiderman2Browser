#include "Renderer.h"
#include "Texture.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <utility>
#include <unordered_map>

namespace fs = std::filesystem;

#include "RendererCore.inl"
#include "RendererDraw.inl"
#include "RendererInstancing.inl"
