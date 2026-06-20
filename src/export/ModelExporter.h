#pragma once

#include "Animation.h"
#include "Skeleton.h"
#include "XBXParser.h"
#include <string>
#include <vector>

namespace model_export {

struct ExportRequest {
    const XBXModel* model = nullptr;
    const Skeleton* skeleton = nullptr;
    const SkeletonAnimMeta* skel_meta = nullptr;
    std::vector<AnimClip>* animations = nullptr;
    std::string output_path;
    bool minion_lizard = false;
};

bool export_glb(const ExportRequest& req, std::string& error);
bool export_fbx(const ExportRequest& req, std::string& error);

}
