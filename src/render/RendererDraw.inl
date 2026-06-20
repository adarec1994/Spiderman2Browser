GPUSkeleton* Renderer::upload_skeleton(const Skeleton* sk) {
    if (!sk) return nullptr;
    auto* gs = new GPUSkeleton();
    gs->build(*sk);
    return gs;
}

void Renderer::draw_scene(const Camera& cam, int vp_x, int vp_w, int vp_h,
                          const GPUModel* model, const GPUSkeleton* skel) {
    glViewport(vp_x,0,vp_w,vp_h);
    glScissor(vp_x,0,vp_w,vp_h);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    if (!model) { glDisable(GL_SCISSOR_TEST); return; }

    float aspect = (float)vp_w / (float)std::max(vp_h,1);

    glm::mat4 S  = glm::scale(glm::mat4(1), glm::vec3(1.f/model->scale));
    glm::mat4 T  = glm::translate(glm::mat4(1), -model->center);
    glm::mat4 Ry = glm::rotate(glm::mat4(1), model_rot_y, glm::vec3(0,1,0));
    glm::mat4 MVP = cam.proj(aspect) * cam.view() * S * Ry * T;
    glm::mat4 M   = S * Ry * T;

    glUseProgram(m_shader);
    glUniform1i(uloc("uInstanced"), 0);
    glUniformMatrix4fv(uloc("uMVP"),  1, GL_FALSE, glm::value_ptr(MVP));
    glUniformMatrix4fv(uloc("uModel"),1, GL_FALSE, glm::value_ptr(M));
    glUniform1i(uloc("uTex"),0);
    glUniform1i(uloc("uWire"),0);
    glUniform1i(uloc("uShowUV"), show_uv ? 1 : 0);
    glUniform1f(uloc("uPointSize"),5.f);
    glActiveTexture(GL_TEXTURE0);

    
    if (m_bones_dirty) {
        glUniformMatrix4fv(uloc("uBones"), 60, GL_FALSE, glm::value_ptr(m_bone_mats[0]));
        m_bones_dirty = false;
    } else {
        glUniformMatrix4fv(uloc("uBones"), 60, GL_FALSE, glm::value_ptr(m_bone_mats[0]));
    }

    
    bool is_skinned = false;
    for (int i=0;i<60;++i) if (m_bone_mats[i] != glm::mat4(1.f)) { is_skinned=true; break; }
    glUniform1i(uloc("uSkinned"), is_skinned ? 1 : 0);

    
    for (auto& mesh : model->meshes) {
        glUniform1i(uloc("uHasTex"), mesh.tex_id ? 1 : 0);
        glUniform1i(uloc("uTranslucent"), mesh.translucent ? 1 : 0);
        mesh.draw();
    }

    
    if (sel_submesh >= 0 && sel_submesh < (int)model->meshes.size()) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glUniform1i(uloc("uWire"), 1);
        glUniform3f(uloc("uWireCol"), 0.1f, 1.0f, 0.3f);
        glLineWidth(2.5f);
        glUniform1i(uloc("uHasTex"), 0);
        model->meshes[sel_submesh].draw();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glLineWidth(1.f);
    }

    
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
        glUniform1i(uloc("uWire"),1);
        glUniform3f(uloc("uWireCol"),0.2f,0.9f,0.4f);
        glLineWidth(1.2f);
        for (auto& mesh:model->meshes) mesh.draw();
        glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    }

    
    if (show_skel && skel && skel->n > 0) {
        glm::mat4 skelMVP = cam.proj(aspect) * cam.view() * S * T; 
        glUniformMatrix4fv(uloc("uMVP"),1,GL_FALSE,glm::value_ptr(skelMVP));
        glUniform1i(uloc("uWire"),1);
        glUniform1i(uloc("uSkinned"),0); 

        glDisable(GL_DEPTH_TEST);

        glLineWidth(1.5f);
        glUniform3f(uloc("uWireCol"),0.45f,0.65f,1.0f);
        skel->draw();
        glLineWidth(1.f);

        glEnable(GL_PROGRAM_POINT_SIZE);

        glUniform1f(uloc("uPointSize"),5.f);
        glUniform3f(uloc("uWireCol"),0.8f,0.8f,0.8f);
        skel->draw_joints();

        if (sel_bone >= 0) {
            glUniform1f(uloc("uPointSize"),10.f);
            glUniform3f(uloc("uWireCol"),1.f,0.55f,0.05f);
            skel->draw_joint(sel_bone);
        }
        glDisable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_DEPTH_TEST);
        glUniformMatrix4fv(uloc("uMVP"),1,GL_FALSE,glm::value_ptr(MVP));
    }

    
    if (show_grid) {
        glm::mat4 gridMVP = cam.proj(aspect)*cam.view();
        glUniformMatrix4fv(uloc("uMVP"),1,GL_FALSE,glm::value_ptr(gridMVP));
        glUniform1i(uloc("uWire"),1);
        glUniform1i(uloc("uSkinned"),0);
        glUniform3f(uloc("uWireCol"),0.22f,0.22f,0.26f);
        glBindVertexArray(m_grid_vao);
        glDrawArrays(GL_LINES,0,m_grid_n);
        glBindVertexArray(0);
    }

    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}






void Renderer::draw_world_instances(const Camera& cam, int vp_x, int vp_w, int vp_h,
                                    const std::vector<std::pair<GPUModel*, glm::mat4>>& instances) {
    glViewport(vp_x, 0, vp_w, vp_h);
    glScissor (vp_x, 0, vp_w, vp_h);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)vp_w / (float)std::max(vp_h, 1);
    glm::mat4 VP = cam.proj(aspect) * cam.view();

    
    
    glm::mat4 VPt = glm::transpose(VP);
    glm::vec4 planes[6] = {
        VPt[3] + VPt[0], 
        VPt[3] - VPt[0], 
        VPt[3] + VPt[1], 
        VPt[3] - VPt[1], 
        VPt[3] + VPt[2], 
        VPt[3] - VPt[2], 
    };
    
    for (auto& p : planes) {
        float len = glm::length(glm::vec3(p));
        if (len > 0.f) p /= len;
    }

    auto in_frustum = [&](glm::vec3 pos, float radius) {
        for (auto& p : planes)
            if (p.x*pos.x + p.y*pos.y + p.z*pos.z + p.w < -radius)
                return false;
        return true;
    };

    glUseProgram(m_shader);
    glUniform1i(uloc("uTex"),     0);
    glUniform1i(uloc("uWire"),    0);
    glUniform1i(uloc("uShowUV"),  0);
    glUniform1i(uloc("uSkinned"), 0);
    glUniform1i(uloc("uInstanced"), 0);
    glUniform1f(uloc("uPointSize"), 1.f);

    
    static glm::mat4 id_bones[60];
    static bool id_init = false;
    if (!id_init) { for (auto& m : id_bones) m = glm::mat4(1.f); id_init = true; }
    glUniformMatrix4fv(uloc("uBones"), 60, GL_FALSE, glm::value_ptr(id_bones[0]));

    glActiveTexture(GL_TEXTURE0);

    unsigned int last_tex = ~0u;
    int drawn = 0, culled = 0;
    for (auto& [model, xform] : instances) {
        if (!model) continue;

        
        glm::vec3 pos = glm::vec3(xform[3]);
        float radius  = model->scale * 2.0f; 
        if (!in_frustum(pos, radius)) { ++culled; continue; }
        ++drawn;
        glm::mat4 MVP = VP * xform;
        glUniformMatrix4fv(uloc("uMVP"),   1, GL_FALSE, glm::value_ptr(MVP));
        glUniformMatrix4fv(uloc("uModel"), 1, GL_FALSE, glm::value_ptr(xform));

        for (auto& mesh : model->meshes) {
            glUniform1i(uloc("uHasTex"), mesh.tex_id ? 1 : 0);
            glUniform1i(uloc("uTranslucent"), mesh.translucent ? 1 : 0);
            if (mesh.tex_id != last_tex) {
                glBindTexture(GL_TEXTURE_2D, mesh.tex_id);
                last_tex = mesh.tex_id;
            }
            mesh.draw();
        }

        if (wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glUniform1i(uloc("uWire"), 1);
            glUniform3f(uloc("uWireCol"), 0.2f, 0.9f, 0.4f);
            glLineWidth(1.0f);
            for (auto& mesh : model->meshes) mesh.draw();
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glUniform1i(uloc("uWire"), 0);
        }
    }

    
    if (show_grid) {
        glm::mat4 gridMVP = VP;
        glUniformMatrix4fv(uloc("uMVP"),   1, GL_FALSE, glm::value_ptr(gridMVP));
        glUniform1i(uloc("uWire"),    1);
        glUniform1i(uloc("uSkinned"), 0);
        glUniform3f(uloc("uWireCol"), 0.22f, 0.22f, 0.26f);
        glBindVertexArray(m_grid_vao);
        glDrawArrays(GL_LINES, 0, m_grid_n);
        glBindVertexArray(0);
    }

    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}
