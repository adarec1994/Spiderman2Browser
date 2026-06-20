#include "UI.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

int UI::PANEL_W = 300;

#include "UIConfig.inl"
#include "UIDraw.inl"
#include "UIMaterial.inl"
