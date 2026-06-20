static void fbx_array(std::ostringstream& o, const std::string& name, const std::vector<double>& vals) {
    o << "\t\t" << name << ": *" << vals.size() << " {\n\t\t\ta: ";
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i) o << ",";
        o << number(vals[i]);
    }
    o << "\n\t\t}\n";
}

static void fbx_array_i(std::ostringstream& o, const std::string& name, const std::vector<int64_t>& vals) {
    o << "\t\t" << name << ": *" << vals.size() << " {\n\t\t\ta: ";
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i) o << ",";
        o << vals[i];
    }
    o << "\n\t\t}\n";
}

struct FbxTRS {
    glm::vec3 t{0.0f};
    glm::vec3 r_deg{0.0f};
    glm::vec3 s{1.0f};
    glm::mat4 matrix{1.0f};
};

static glm::vec3 extract_xyz_degrees(const glm::mat4& m) {
    float x = std::atan2(m[1][2], m[2][2]);
    float y = std::atan2(-m[0][2], std::sqrt(m[1][2] * m[1][2] + m[2][2] * m[2][2]));
    float z = std::atan2(m[0][1], m[0][0]);
    glm::vec3 e = glm::degrees(glm::vec3(x, y, z));
    if (!std::isfinite(e.x)) e.x = 0.0f;
    if (!std::isfinite(e.y)) e.y = 0.0f;
    if (!std::isfinite(e.z)) e.z = 0.0f;
    return e;
}

static glm::mat3 orthonormal_basis(glm::vec3 x, glm::vec3 y, glm::vec3 z) {
    if (glm::dot(x, x) < 1e-12f) x = glm::vec3(1, 0, 0); else x = glm::normalize(x);
    y = y - x * glm::dot(x, y);
    if (glm::dot(y, y) < 1e-12f) {
        y = glm::abs(x.y) < 0.9f ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), x))
                                 : glm::normalize(glm::cross(glm::vec3(1, 0, 0), x));
    } else {
        y = glm::normalize(y);
    }
    z = glm::cross(x, y);
    if (glm::dot(z, z) < 1e-12f) z = glm::vec3(0, 0, 1); else z = glm::normalize(z);
    y = glm::normalize(glm::cross(z, x));
    return glm::mat3(x, y, z);
}

static FbxTRS decompose_fbx_trs(const glm::mat4& m) {
    FbxTRS out;
    out.t = glm::vec3(m[3]);
    glm::vec3 x(m[0]);
    glm::vec3 y(m[1]);
    glm::vec3 z(m[2]);
    out.s = glm::vec3(std::sqrt(std::max(0.0f, glm::dot(x, x))),
                      std::sqrt(std::max(0.0f, glm::dot(y, y))),
                      std::sqrt(std::max(0.0f, glm::dot(z, z))));
    if (out.s.x < 1e-8f) out.s.x = 1.0f;
    if (out.s.y < 1e-8f) out.s.y = 1.0f;
    if (out.s.z < 1e-8f) out.s.z = 1.0f;
    glm::mat3 basis(x / out.s.x, y / out.s.y, z / out.s.z);
    if (glm::determinant(basis) < 0.0f) {
        out.s.x = -out.s.x;
        basis[0] = -basis[0];
    }
    basis = orthonormal_basis(basis[0], basis[1], basis[2]);
    glm::mat4 r(1.0f);
    r[0] = glm::vec4(basis[0], 0.0f);
    r[1] = glm::vec4(basis[1], 0.0f);
    r[2] = glm::vec4(basis[2], 0.0f);
    out.r_deg = extract_xyz_degrees(r);
    out.matrix = glm::mat4(1.0f);
    out.matrix[0] = glm::vec4(basis[0] * out.s.x, 0.0f);
    out.matrix[1] = glm::vec4(basis[1] * out.s.y, 0.0f);
    out.matrix[2] = glm::vec4(basis[2] * out.s.z, 0.0f);
    out.matrix[3] = glm::vec4(out.t, 1.0f);
    return out;
}

static FbxTRS fbx_identity_trs() {
    return decompose_fbx_trs(glm::mat4(1.0f));
}

static glm::mat4 fbx_export_root_matrix() {
    glm::mat4 m(1.0f);
    m[0][0] = -1.0f;
    m[2][2] = -1.0f;
    return m;
}

static FbxTRS fbx_quat_trs(const glm::quat& q, const glm::mat4& pre = glm::mat4(1.0f)) {
    return decompose_fbx_trs(pre * glm::mat4_cast(safe_quat(q)));
}

static double unwrap_degrees(double value, double reference) {
    while (value - reference > 180.0) value -= 360.0;
    while (value - reference < -180.0) value += 360.0;
    return value;
}

static std::vector<glm::vec3> fbx_euler_curve(const std::vector<glm::quat>& rotations,
                                              const glm::mat4& pre = glm::mat4(1.0f)) {
    std::vector<glm::vec3> out;
    out.reserve(rotations.size());
    glm::quat prev_q(1, 0, 0, 0);
    glm::vec3 prev_e(0.0f);
    bool first = true;
    for (glm::quat q : rotations) {
        q = safe_quat(q);
        if (!first && glm::dot(prev_q, q) < 0.0f)
            q = -q;
        glm::vec3 base = fbx_quat_trs(q, pre).r_deg;
        glm::vec3 e = base;
        if (!first) {
            std::array<glm::vec3, 2> branches = {
                base,
                glm::vec3(base.x + 180.0f, 180.0f - base.y, base.z + 180.0f)
            };
            double best = std::numeric_limits<double>::max();
            for (glm::vec3 cand : branches) {
                cand.x = (float)unwrap_degrees(cand.x, prev_e.x);
                cand.y = (float)unwrap_degrees(cand.y, prev_e.y);
                cand.z = (float)unwrap_degrees(cand.z, prev_e.z);
                glm::vec3 d = cand - prev_e;
                double score = (double)d.x * d.x + (double)d.y * d.y + (double)d.z * d.z;
                if (score < best) {
                    best = score;
                    e = cand;
                }
            }
        }
        out.push_back(e);
        prev_q = q;
        prev_e = e;
        first = false;
    }
    return out;
}

static void fbx_props_transform(std::ostringstream& o, const FbxTRS& trs) {
    o << "\t\tProperties70:  {\n";
    o << "\t\t\tP: \"RotationActive\", \"bool\", \"\", \"\",1\n";
    o << "\t\t\tP: \"RotationOrder\", \"enum\", \"\", \"\",0\n";
    o << "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n";
    o << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\","
      << number(trs.t.x) << "," << number(trs.t.y) << "," << number(trs.t.z) << "\n";
    o << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\","
      << number(trs.r_deg.x) << "," << number(trs.r_deg.y) << "," << number(trs.r_deg.z) << "\n";
    o << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\","
      << number(trs.s.x) << "," << number(trs.s.y) << "," << number(trs.s.z) << "\n";
    o << "\t\t}\n";
}

static std::vector<glm::mat4> exported_world_bind(const ExportRequest& req,
                                                  const std::vector<glm::mat4>& local_bind,
                                                  int bone_count) {
    std::vector<glm::mat4> local_fbx(bone_count, glm::mat4(1.0f));
    std::vector<glm::mat4> world(bone_count, glm::mat4(1.0f));
    for (int i = 0; i < bone_count; ++i)
        local_fbx[i] = decompose_fbx_trs(local_bind[i]).matrix;
    for (int i = 0; i < bone_count; ++i) {
        int parent = req.skeleton ? req.skeleton->bones[i].parent : -1;
        if (parent >= 0 && parent < i)
            world[i] = world[parent] * local_fbx[i];
        else
            world[i] = local_fbx[i];
    }
    return world;
}

static std::vector<double> fbx_matrix(const glm::mat4& m) {
    std::vector<double> v;
    v.reserve(16);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            v.push_back(m[c][r]);
    return v;
}
