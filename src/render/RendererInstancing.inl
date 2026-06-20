void InstancedModel::release() {
    if (inst_vbo) glDeleteBuffers(1, &inst_vbo);
    inst_vbo = 0;
    xforms.clear();
    model = nullptr;   
}
void InstancedWorld::release() {
    for (auto& im : models) im.release();
    models.clear();
    total_instances = total_draws = 0;
}




static void attach_instance_buffer(GPUModel* model, unsigned int inst_vbo) {
    for (auto& mesh : model->meshes) {
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
        for (int c = 0; c < 4; ++c) {
            glEnableVertexAttribArray(4 + c);
            glVertexAttribPointer(4 + c, 4, GL_FLOAT, GL_FALSE,
                                  (GLsizei)sizeof(glm::mat4),
                                  (void*)(uintptr_t)(c * sizeof(glm::vec4)));
            glVertexAttribDivisor(4 + c, 1);   
        }
        glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

InstancedWorld Renderer::build_instanced_world(
        const std::vector<WorldPlacement>& instances) {
    InstancedWorld iw;

    
    
    
    std::unordered_map<uint64_t, size_t> index_of;   
    for (auto& pl : instances) {
        if (!pl.model) continue;
        uint64_t key = (uint64_t)(uintptr_t)pl.model ^ ((uint64_t)pl.tex_override << 1);
        auto it = index_of.find(key);
        if (it == index_of.end()) {
            index_of[key] = iw.models.size();
            InstancedModel im; im.model = pl.model; im.tex_override = pl.tex_override;
            im.xforms.push_back(pl.xform);
            iw.models.push_back(std::move(im));
        } else {
            iw.models[it->second].xforms.push_back(pl.xform);
        }
    }

    
    
    
    
    for (auto& im : iw.models) {
        if (!im.model || im.xforms.empty()) continue;
        glGenBuffers(1, &im.inst_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, im.inst_vbo);
        glBufferData(GL_ARRAY_BUFFER, im.xforms.size() * sizeof(glm::mat4),
                     im.xforms.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        iw.total_instances += (long long)im.xforms.size();
        iw.total_draws     += (long long)im.model->meshes.size();   
    }

    return iw;
}

void Renderer::draw_instanced_world(const Camera& cam, int vp_x, int vp_w, int vp_h,
                                    const InstancedWorld& iw) {
    glViewport(vp_x, 0, vp_w, vp_h);
    glScissor (vp_x, 0, vp_w, vp_h);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)vp_w / (float)std::max(vp_h, 1);
    glm::mat4 VP = cam.proj(aspect) * cam.view();

    glUseProgram(m_shader);
    glUniform1i(uloc("uTex"),     0);
    glUniform1i(uloc("uWire"),    0);
    glUniform1i(uloc("uShowUV"),  0);
    glUniform1i(uloc("uSkinned"), 0);
    glUniform1i(uloc("uInstanced"), 1);                
    glUniform1f(uloc("uPointSize"), 1.f);
    glUniformMatrix4fv(uloc("uVP"), 1, GL_FALSE, glm::value_ptr(VP));

    glActiveTexture(GL_TEXTURE0);

    auto draw_pass = [&](bool translucent_pass) {
        unsigned int last_tex = ~0u;
        for (auto& im : iw.models) {
            if (!im.model || im.xforms.empty()) continue;
            const GLsizei ninst = (GLsizei)im.xforms.size();
            
            attach_instance_buffer(im.model, im.inst_vbo);
            for (auto& mesh : im.model->meshes) {
                if (mesh.translucent != translucent_pass) continue;
                
                
                unsigned int tex = (im.tex_override && mesh.is_blg) ? im.tex_override : mesh.tex_id;
                glUniform1i(uloc("uHasTex"), tex ? 1 : 0);
                glUniform1i(uloc("uTranslucent"), mesh.translucent ? 1 : 0);
                if (tex != last_tex) { glBindTexture(GL_TEXTURE_2D, tex); last_tex = tex; }
                glBindVertexArray(mesh.vao);
                glDrawElementsInstanced(GL_TRIANGLES, mesh.n_indices, GL_UNSIGNED_INT, nullptr, ninst);
            }
        }
    };
    draw_pass(false);   
    
    
    
    
    
    glDepthMask(GL_FALSE);
    draw_pass(true);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);

    glUniform1i(uloc("uInstanced"), 0);   

    if (show_grid) {
        glUniformMatrix4fv(uloc("uMVP"), 1, GL_FALSE, glm::value_ptr(VP));
        glUniform1i(uloc("uWire"), 1);
        glUniform3f(uloc("uWireCol"), 0.22f, 0.22f, 0.26f);
        glBindVertexArray(m_grid_vao);
        glDrawArrays(GL_LINES, 0, m_grid_n);
        glBindVertexArray(0);
        glUniform1i(uloc("uWire"), 0);
    }

    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}
