#include "ModelExporter.h"
#include "Texture.h"
#include "Vfs.h"

#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace model_export {
namespace {
#include "ModelExporterCommon.inl"
#include "ModelExporterGlbHelpers.inl"
#include "ModelExporterFbxHelpers.inl"

}

#include "ModelExporterGlb.inl"
#include "ModelExporterFbx.inl"

}
