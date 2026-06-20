bool export_fbx(const ExportRequest& req, std::string& error) {
    error.clear();
    if (!req.model || req.output_path.empty()) {
        error = "No model or output path";
        return false;
    }
    fs::path out = req.output_path;
    std::error_code ec;
    if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
    std::vector<int> mat_texture;
    std::vector<TextureRef> textures = export_material_textures(req, mat_texture, error);
    if (!error.empty()) return false;
    int bone_count = 0;
    if (req.skeleton && req.model->bind_pose.size() > 0)
        bone_count = std::min({(int)req.skeleton->bones.size(), (int)req.model->bind_pose.size(), 60});
    std::vector<glm::mat4> local_bind = local_bind_matrices(req, bone_count);
    glm::mat4 fbx_root_matrix = fbx_export_root_matrix();
    std::vector<glm::mat4> fbx_bind_world(bone_count, glm::mat4(1.0f));
    for (int i = 0; i < bone_count; ++i)
        fbx_bind_world[i] = fbx_root_matrix * req.model->bind_pose[i];
    std::vector<SampledClip> sampled = sample_clips(req, bone_count, local_bind);

    struct FbxFace {
        uint32_t v[3] = {};
        glm::vec2 uv[3] = {};
    };
    struct FbxSubmesh {
        int source_index = -1;
        std::string name;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normal_accum;
        std::vector<FbxFace> faces;
        std::vector<double> verts;
        std::vector<int64_t> poly;
        std::vector<double> normals;
        std::vector<double> uvs;
        std::vector<int64_t> uv_idx;
        std::vector<glm::ivec4> vertex_joints;
        std::vector<glm::vec4> vertex_weights;
        std::vector<std::vector<int64_t>> cluster_indexes;
        std::vector<std::vector<double>> cluster_weights;
        std::vector<int64_t> cluster_ids;
        int64_t geom_id = 0;
        int64_t mesh_id = 0;
        int64_t skin_id = 0;
        int64_t material_id = 0;
    };
    std::vector<FbxSubmesh> fbx_submeshes;

    for (int si = 0; si < (int)req.model->submeshes.size(); ++si) {
        const auto& sm = req.model->submeshes[si];
        if (sm.positions.empty() || sm.indices.empty()) continue;
        FbxSubmesh out_sm;
        out_sm.source_index = si;
        out_sm.name = clean_name(fs::path(req.model->filepath).stem().string() + "_SM" + std::to_string(si) + "_" +
                                 (sm.mat_name.empty() ? std::string("material") : sm.mat_name));
        out_sm.positions.reserve(sm.positions.size());
        out_sm.normal_accum.assign(sm.positions.size(), glm::vec3(0.0f));
        out_sm.vertex_joints.reserve(sm.positions.size());
        out_sm.vertex_weights.reserve(sm.positions.size());
        for (size_t vi = 0; vi < sm.positions.size(); ++vi) {
            glm::vec3 p = glm::vec3(fbx_root_matrix * glm::vec4(sm.positions[vi], 1.0f));
            out_sm.positions.push_back(p);
            out_sm.verts.push_back(p.x);
            out_sm.verts.push_back(p.y);
            out_sm.verts.push_back(p.z);
            if (bone_count > 0 && vi < sm.bone_indices.size() && vi < sm.bone_weights.size()) {
                glm::ivec4 ji = sm.bone_indices[vi];
                glm::vec4 jw = sm.bone_weights[vi];
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    ji[k] = std::clamp(ji[k], 0, bone_count - 1);
                    if (!std::isfinite(jw[k]) || jw[k] < 0.0f) jw[k] = 0.0f;
                    sum += jw[k];
                }
                if (sum > 0.0001f)
                    jw /= sum;
                else {
                    ji = glm::ivec4(0, 0, 0, 0);
                    jw = glm::vec4(1, 0, 0, 0);
                }
                out_sm.vertex_joints.push_back(ji);
                out_sm.vertex_weights.push_back(jw);
            } else {
                out_sm.vertex_joints.push_back(glm::ivec4(0, 0, 0, 0));
                out_sm.vertex_weights.push_back(glm::vec4(1, 0, 0, 0));
            }
        }
        for (size_t k = 0; k + 2 < sm.indices.size(); k += 3) {
            uint32_t ids[3] = {sm.indices[k], sm.indices[k + 1], sm.indices[k + 2]};
            FbxFace face;
            bool ok_face = true;
            for (int corner = 0; corner < 3; ++corner) {
                uint32_t idx = ids[corner];
                if (idx >= sm.positions.size()) {
                    ok_face = false;
                    break;
                }
                face.v[corner] = idx;
                glm::vec2 uv(0.0f);
                if (idx < sm.uvs.size()) uv = glm::vec2(sm.uvs[idx].x, 1.0f - sm.uvs[idx].y);
                face.uv[corner] = uv;
            }
            if (!ok_face) continue;
            glm::vec3 p0 = out_sm.positions[face.v[0]];
            glm::vec3 p1 = out_sm.positions[face.v[1]];
            glm::vec3 p2 = out_sm.positions[face.v[2]];
            glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
            if (std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z) &&
                glm::dot(n, n) > 1e-16f) {
                n = glm::normalize(n);
                out_sm.normal_accum[face.v[0]] += n;
                out_sm.normal_accum[face.v[1]] += n;
                out_sm.normal_accum[face.v[2]] += n;
            }
            out_sm.faces.push_back(face);
        }
        if (out_sm.faces.empty()) continue;
        for (glm::vec3& n : out_sm.normal_accum) {
            if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) ||
                glm::dot(n, n) <= 1e-16f)
                n = glm::vec3(0, 1, 0);
            else
                n = glm::normalize(n);
        }
        for (const FbxFace& face : out_sm.faces) {
            out_sm.poly.push_back((int64_t)face.v[0]);
            out_sm.poly.push_back((int64_t)face.v[1]);
            out_sm.poly.push_back(-(int64_t)face.v[2] - 1);
            for (int corner = 0; corner < 3; ++corner) {
                glm::vec3 n = out_sm.normal_accum[face.v[corner]];
                out_sm.normals.push_back(n.x);
                out_sm.normals.push_back(n.y);
                out_sm.normals.push_back(n.z);
                out_sm.uvs.push_back(face.uv[corner].x);
                out_sm.uvs.push_back(face.uv[corner].y);
                out_sm.uv_idx.push_back((int64_t)out_sm.uv_idx.size());
            }
        }
        out_sm.cluster_indexes.assign(bone_count, {});
        out_sm.cluster_weights.assign(bone_count, {});
        out_sm.cluster_ids.assign(bone_count, 0);
        if (bone_count > 0) {
            for (int vi = 0; vi < (int)out_sm.vertex_joints.size(); ++vi) {
                for (int bi = 0; bi < bone_count; ++bi) {
                    double w = 0.0;
                    for (int k = 0; k < 4; ++k)
                        if (out_sm.vertex_joints[vi][k] == bi)
                            w += out_sm.vertex_weights[vi][k];
                    if (w > 0.0001) {
                        out_sm.cluster_indexes[bi].push_back(vi);
                        out_sm.cluster_weights[bi].push_back(w);
                    }
                }
            }
        }
        fbx_submeshes.push_back(std::move(out_sm));
    }

    if (fbx_submeshes.empty()) {
        error = "Model has no exportable FBX geometry";
        return false;
    }

    std::ostringstream o;
    o.imbue(std::locale::classic());
    std::vector<std::string> conns;
    int64_t id = 100000;
    auto next_id = [&](){ return id++; };
    int64_t armature_id = next_id();
    int64_t armature_attr_id = next_id();
    int64_t bind_pose_id = bone_count > 0 ? next_id() : 0;
    std::vector<int64_t> bone_ids;
    std::vector<int64_t> bone_attr_ids;
    std::vector<int64_t> tex_ids(textures.size()), video_ids(textures.size());
    o << "; FBX 7.4.0 project file\n";
    o << "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n";
    o << "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n\t}\n}\n";
    o << "Objects:  {\n";

    o << "\tNodeAttribute: " << armature_attr_id << ", \"NodeAttribute::Armature\", \"Null\" {\n\t\tTypeFlags: \"Null\"\n\t}\n";
    o << "\tModel: " << armature_id << ", \"Model::Armature\", \"Null\" {\n\t\tVersion: 232\n";
    fbx_props_transform(o, fbx_identity_trs());
    o << "\t}\n";
    conns.push_back("C: \"OO\"," + std::to_string(armature_attr_id) + "," + std::to_string(armature_id));
    conns.push_back("C: \"OO\"," + std::to_string(armature_id) + ",0");

    for (size_t i = 0; i < textures.size(); ++i) {
        video_ids[i] = next_id();
        tex_ids[i] = next_id();
        fs::path tp = textures[i].absolute_path;
        o << "\tVideo: " << video_ids[i] << ", \"Video::" << json_escape(tp.filename().string()) << "\", \"Clip\" {\n";
        o << "\t\tType: \"Clip\"\n\t\tFileName: \"" << json_escape(tp.string()) << "\"\n";
        o << "\t\tRelativeFilename: \"" << json_escape(textures[i].uri) << "\"\n\t\tUseMipMap: 0\n\t}\n";
        o << "\tTexture: " << tex_ids[i] << ", \"Texture::" << json_escape(tp.filename().string()) << "\", \"\" {\n";
        o << "\t\tType: \"TextureVideoClip\"\n\t\tVersion: 202\n\t\tTextureName: \"Texture::" << json_escape(tp.filename().string()) << "\"\n";
        o << "\t\tFileName: \"" << json_escape(tp.string()) << "\"\n\t\tRelativeFilename: \"" << json_escape(textures[i].uri) << "\"\n";
        o << "\t\tMedia: \"Video::" << json_escape(tp.filename().string()) << "\"\n";
        o << "\t\tTexture_Alpha_Source: \"None\"\n";
        o << "\t\tModelUVTranslation: 0,0\n\t\tModelUVScaling: 1,1\n\t\tCropping: 0,0,0,0\n";
        o << "\t\tProperties70:  {\n\t\t\tP: \"UVSet\", \"KString\", \"\", \"\", \"UVChannel_1\"\n\t\t\tP: \"UseMaterial\", \"bool\", \"\", \"\",1\n\t\t}\n\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(video_ids[i]) + "," + std::to_string(tex_ids[i]));
    }
    for (int i = 0; i < bone_count; ++i) {
        bone_ids.push_back(next_id());
        bone_attr_ids.push_back(next_id());
        std::string name = req.skeleton->bones[i].name.empty() ? ("bone_" + std::to_string(i)) : req.skeleton->bones[i].name;
        int p = req.skeleton->bones[i].parent;
        FbxTRS trs = decompose_fbx_trs((p < 0 || p >= bone_count) ? fbx_root_matrix * local_bind[i] : local_bind[i]);
        o << "\tNodeAttribute: " << bone_attr_ids.back() << ", \"NodeAttribute::" << json_escape(name) << "\", \"LimbNode\" {\n";
        o << "\t\tTypeFlags: \"Skeleton\"\n\t\tSkeletonType: \"LimbNode\"\n\t\tSize: 1\n\t}\n";
        o << "\tModel: " << bone_ids.back() << ", \"Model::" << json_escape(name) << "\", \"LimbNode\" {\n";
        o << "\t\tVersion: 232\n";
        fbx_props_transform(o, trs);
        o << "\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(bone_attr_ids[i]) + "," + std::to_string(bone_ids[i]));
        if (p >= 0 && p < i)
            conns.push_back("C: \"OO\"," + std::to_string(bone_ids[i]) + "," + std::to_string(bone_ids[p]));
        else
            conns.push_back("C: \"OO\"," + std::to_string(bone_ids[i]) + "," + std::to_string(armature_id));
    }

    for (FbxSubmesh& sm_out : fbx_submeshes) {
        sm_out.geom_id = next_id();
        sm_out.mesh_id = next_id();
        sm_out.material_id = next_id();
        if (bone_count > 0) {
            sm_out.skin_id = next_id();
            for (int bi = 0; bi < bone_count; ++bi)
                if (!sm_out.cluster_indexes[bi].empty())
                    sm_out.cluster_ids[bi] = next_id();
        }
    }

    for (FbxSubmesh& sm_out : fbx_submeshes) {
        const auto& src_sm = req.model->submeshes[sm_out.source_index];
        o << "\tGeometry: " << sm_out.geom_id << ", \"Geometry::" << sm_out.name << "\", \"Mesh\" {\n";
        fbx_array(o, "Vertices", sm_out.verts);
        fbx_array_i(o, "PolygonVertexIndex", sm_out.poly);
        o << "\t\tGeometryVersion: 124\n";
        o << "\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"Direct\"\n";
        fbx_array(o, "Normals", sm_out.normals);
        o << "\t\t}\n";
        o << "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVChannel_1\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"Direct\"\n";
        fbx_array(o, "UV", sm_out.uvs);
        o << "\t\t}\n";
        o << "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tMappingInformationType: \"AllSame\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *1 {\n\t\t\t\ta: 0\n\t\t\t}\n\t\t}\n";
        o << "\t\tLayer: 0 {\n\t\t\tVersion: 100\n\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t}\n";
        o << "\t}\n";
        o << "\tModel: " << sm_out.mesh_id << ", \"Model::" << sm_out.name << "\", \"Mesh\" {\n\t\tVersion: 232\n";
        fbx_props_transform(o, fbx_identity_trs());
        o << "\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n";
        std::string mat_name = clean_name(src_sm.mat_name.empty() ? ("material_" + std::to_string(sm_out.source_index)) : src_sm.mat_name);
        o << "\tMaterial: " << sm_out.material_id << ", \"Material::" << json_escape(mat_name) << "\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t\tMultiLayer: 0\n\t\tProperties70:  {\n\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",1,1,1\n\t\t\tP: \"Diffuse\", \"Vector3D\", \"Vector\", \"\",1,1,1\n\t\t\tP: \"AmbientColor\", \"Color\", \"\", \"A\",0,0,0\n\t\t\tP: \"SpecularColor\", \"Color\", \"\", \"A\",0,0,0\n\t\t\tP: \"Opacity\", \"double\", \"Number\", \"\",1\n\t\t\tP: \"TransparencyFactor\", \"double\", \"Number\", \"\",0\n\t\t}\n\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(sm_out.geom_id) + "," + std::to_string(sm_out.mesh_id));
        conns.push_back("C: \"OO\"," + std::to_string(sm_out.mesh_id) + "," + std::to_string(armature_id));
        conns.push_back("C: \"OO\"," + std::to_string(sm_out.material_id) + "," + std::to_string(sm_out.mesh_id));
        int tr = sm_out.source_index < (int)mat_texture.size() ? mat_texture[sm_out.source_index] : -1;
        if (tr >= 0)
            conns.push_back("C: \"OP\"," + std::to_string(tex_ids[tr]) + "," + std::to_string(sm_out.material_id) + ", \"DiffuseColor\"");
        if (bone_count > 0) {
            o << "\tDeformer: " << sm_out.skin_id << ", \"Deformer::" << sm_out.name << "_skin\", \"Skin\" {\n\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n";
            conns.push_back("C: \"OO\"," + std::to_string(sm_out.skin_id) + "," + std::to_string(sm_out.geom_id));
            for (int bi = 0; bi < bone_count; ++bi) {
                if (sm_out.cluster_indexes[bi].empty()) continue;
                std::string name = req.skeleton->bones[bi].name.empty() ? ("bone_" + std::to_string(bi)) : req.skeleton->bones[bi].name;
                o << "\tDeformer: " << sm_out.cluster_ids[bi] << ", \"SubDeformer::" << json_escape(sm_out.name + "_" + name) << "\", \"Cluster\" {\n\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n\t\tLink_Mode: \"Normalize\"\n\t\tLinkMode: \"Normalize\"\n";
                fbx_array_i(o, "Indexes", sm_out.cluster_indexes[bi]);
                fbx_array(o, "Weights", sm_out.cluster_weights[bi]);
                fbx_array(o, "Transform", fbx_matrix(glm::mat4(1.0f)));
                fbx_array(o, "TransformLink", fbx_matrix(fbx_bind_world[bi]));
                o << "\t}\n";
                conns.push_back("C: \"OO\"," + std::to_string(sm_out.cluster_ids[bi]) + "," + std::to_string(sm_out.skin_id));
                conns.push_back("C: \"OO\"," + std::to_string(bone_ids[bi]) + "," + std::to_string(sm_out.cluster_ids[bi]));
            }
        }
    }

    if (bone_count > 0) {
        o << "\tPose: " << bind_pose_id << ", \"Pose::BindPose\", \"BindPose\" {\n\t\tType: \"BindPose\"\n\t\tVersion: 100\n\t\tNbPoseNodes: " << (bone_count + 1 + (int)fbx_submeshes.size()) << "\n";
        o << "\t\tPoseNode:  {\n\t\t\tNode: " << armature_id << "\n";
        fbx_array(o, "Matrix", fbx_matrix(glm::mat4(1.0f)));
        o << "\t\t}\n";
        for (const FbxSubmesh& sm_out : fbx_submeshes) {
            o << "\t\tPoseNode:  {\n\t\t\tNode: " << sm_out.mesh_id << "\n";
            fbx_array(o, "Matrix", fbx_matrix(glm::mat4(1.0f)));
            o << "\t\t}\n";
        }
        for (int bi = 0; bi < bone_count; ++bi) {
            o << "\t\tPoseNode:  {\n\t\t\tNode: " << bone_ids[bi] << "\n";
            fbx_array(o, "Matrix", fbx_matrix(fbx_bind_world[bi]));
            o << "\t\t}\n";
        }
        o << "\t}\n";
    }
    const int64_t ticks_per_second = 46186158000LL;
    for (size_t ai = 0; ai < sampled.size(); ++ai) {
        int64_t stack = next_id();
        int64_t layer = next_id();
        std::string an = clean_name(sampled[ai].name);
        int64_t stop = sampled[ai].times.empty() ? 0 : (int64_t)std::llround(sampled[ai].times.back() * (double)ticks_per_second);
        o << "\tAnimationStack: " << stack << ", \"AnimStack::" << json_escape(an) << "\", \"\" {\n\t\tProperties70:  {\n\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," << stop << "\n\t\t}\n\t}\n";
        o << "\tAnimationLayer: " << layer << ", \"AnimLayer::BaseLayer\", \"\" {\n\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(stack) + ",0");
        conns.push_back("C: \"OO\"," + std::to_string(layer) + "," + std::to_string(stack));
        std::vector<int64_t> kt;
        kt.reserve(sampled[ai].times.size());
        for (float t : sampled[ai].times) kt.push_back((int64_t)std::llround(t * (double)ticks_per_second));
        for (int bi = 0; bi < bone_count; ++bi) {
            int64_t node = next_id();
            o << "\tAnimationCurveNode: " << node << ", \"AnimCurveNode::" << json_escape(req.skeleton->bones[bi].name) << "_Lcl Rotation\", \"\" {\n\t\tProperties70:  {\n\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n\t\t}\n\t}\n";
            conns.push_back("C: \"OO\"," + std::to_string(node) + "," + std::to_string(layer));
            conns.push_back("C: \"OP\"," + std::to_string(node) + "," + std::to_string(bone_ids[bi]) + ", \"Lcl Rotation\"");
            int p = req.skeleton->bones[bi].parent;
            std::vector<glm::vec3> eulers = fbx_euler_curve(sampled[ai].rotations[bi],
                                                            (p < 0 || p >= bone_count) ? fbx_root_matrix : glm::mat4(1.0f));
            for (int axis = 0; axis < 3; ++axis) {
                int64_t curve = next_id();
                std::vector<double> values;
                values.reserve(eulers.size());
                for (const glm::vec3& e : eulers) {
                    values.push_back(axis == 0 ? e.x : (axis == 1 ? e.y : e.z));
                }
                o << "\tAnimationCurve: " << curve << ", \"AnimCurve::\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4008\n";
                fbx_array_i(o, "KeyTime", kt);
                fbx_array(o, "KeyValueFloat", values);
                std::vector<int64_t> flags(1, 24840);
                std::vector<double> attr(4u, 0.0);
                std::vector<int64_t> ref(1, (int64_t)values.size());
                fbx_array_i(o, "KeyAttrFlags", flags);
                fbx_array(o, "KeyAttrDataFloat", attr);
                fbx_array_i(o, "KeyAttrRefCount", ref);
                o << "\t}\n";
                const char* prop = axis == 0 ? "d|X" : (axis == 1 ? "d|Y" : "d|Z");
                conns.push_back("C: \"OP\"," + std::to_string(curve) + "," + std::to_string(node) + ", \"" + prop + "\"");
            }
        }
    }
    o << "}\nConnections:  {\n";
    for (auto& c : conns) o << "\t" << c << "\n";
    o << "}\n";
    std::ofstream f(out, std::ios::binary);
    if (!f) {
        error = "Could not write FBX";
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    if (!f) {
        error = "Could not finish FBX";
        return false;
    }
    return true;
}
