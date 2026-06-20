int App::pick_bone(double mx, double my) {
    if (!m_skeleton || !m_gpu_model) return -1;
    int w = vp_w(m_w), h = m_h;
    glm::vec3 ro = m_cam.eye();
    glm::vec3 rd = m_cam.ray_dir((float)mx,(float)my, vp_x(), w, h);
    float s = m_gpu_model->scale;
    glm::vec3 c = m_gpu_model->center;
    float pick_r = s * 0.03f;

    float best_t = 1e9f;
    int   best_i = -1;
    for (int i = 0; i < (int)m_skeleton->bones.size(); ++i) {
        glm::vec3 bp = (m_skeleton->bones[i].world_pos - c) / s;
        glm::vec3 oc = ro - bp;
        float b    = glm::dot(oc,rd);
        float r    = pick_r/s;
        float disc = b*b - glm::dot(oc,oc) + r*r;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t > 0 && t < best_t) { best_t=t; best_i=i; }
    }
    return best_i;
}


static bool ray_tri(glm::vec3 ro, glm::vec3 rd,
                    glm::vec3 v0, glm::vec3 v1, glm::vec3 v2, float& t) {
    const float EPS = 1e-7f;
    glm::vec3 e1=v1-v0, e2=v2-v0, h=glm::cross(rd,e2);
    float a = glm::dot(e1,h);
    if (fabsf(a)<EPS) return false;
    float f = 1.f/a;
    glm::vec3 s=ro-v0;
    float u = f*glm::dot(s,h);
    if (u<0.f||u>1.f) return false;
    glm::vec3 q=glm::cross(s,e1);
    float v = f*glm::dot(rd,q);
    if (v<0.f||u+v>1.f) return false;
    t = f*glm::dot(e2,q);
    return t>EPS;
}


static std::vector<uint32_t> build_tris_for_pick(
    const std::vector<uint16_t>& raw, uint32_t vc, int method_sel)
{
    std::vector<uint32_t> tris;
    if (!raw.empty()) {
        if (method_sel==1) { 
            for (size_t i=0;i+2<raw.size();i+=3) {
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2];
                if (a<vc&&b<vc&&c<vc&&a!=b&&b!=c&&a!=c){tris.push_back(a);tris.push_back(b);tris.push_back(c);}
            }
        } else if (method_sel==2) { 
            for (size_t i=0;i+3<raw.size();i+=4) {
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2],d=raw[i+3];
                if (a<vc&&b<vc&&c<vc&&d<vc){
                    tris.push_back(a);tris.push_back(b);tris.push_back(c);
                    tris.push_back(a);tris.push_back(c);tris.push_back(d);}
            }
        } else if (method_sel==3) { 
            if (!raw.empty()&&raw[0]<vc)
                for (size_t i=1;i+1<raw.size();++i){
                    uint32_t b=raw[i],c=raw[i+1];
                    if (b<vc&&c<vc&&raw[0]!=b&&b!=c&&raw[0]!=c){tris.push_back(raw[0]);tris.push_back(b);tris.push_back(c);}
                }
        } else { 
            int p=0;
            for (size_t i=0;i+2<raw.size();++i){
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2];
                if (a>=vc||b>=vc||c>=vc){p^=1;continue;}
                if (a==b||b==c||a==c){p^=1;continue;}
                if (p){tris.push_back(a);tris.push_back(c);tris.push_back(b);}
                else  {tris.push_back(a);tris.push_back(b);tris.push_back(c);}
                p^=1;
            }
        }
    } else { 
        int p=0;
        for (uint32_t i=0;i+2<vc;++i){
            if (p){tris.push_back(i);tris.push_back(i+2);tris.push_back(i+1);}
            else  {tris.push_back(i);tris.push_back(i+1);tris.push_back(i+2);}
            p^=1;
        }
    }
    return tris;
}

void App::try_select(double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;

    
    if (m_gpu_model && !m_cached_raw.empty()) {
        int w = vp_w(m_w), h = m_h;
        glm::vec3 ro = m_cam.eye();
        glm::vec3 rd = m_cam.ray_dir((float)mx,(float)my, vp_x(), w, h);

        
        glm::mat4 T    = glm::translate(glm::mat4(1), -m_gpu_model->center);
        glm::mat4 Ry   = glm::rotate(glm::mat4(1), m_model_rot_y, glm::vec3(0,1,0));
        glm::mat4 S    = glm::scale(glm::mat4(1), glm::vec3(1.f/m_gpu_model->scale));
        glm::mat4 Minv = glm::inverse(S * Ry * T);

        
        glm::vec3 lo = glm::vec3(Minv * glm::vec4(ro, 1.f));
        glm::vec3 ld = glm::normalize(glm::vec3(Minv * glm::vec4(rd, 0.f)));

        float best_t  = 1e30f;
        int   best_sm = -1;

        for (int si=0; si<(int)m_cached_raw.size(); ++si) {
            auto& rm = m_cached_raw[si];
            if (rm.positions.empty()) continue;
            int method = (si<(int)m_ui_state.submeshes.size()) ?
                          m_ui_state.submeshes[si].method_sel : 0;
            auto tris = build_tris_for_pick(rm.raw, rm.vc, method);
            for (size_t ti=0; ti+2<tris.size(); ti+=3) {
                float t;
                if (ray_tri(lo,ld, rm.positions[tris[ti]],
                                   rm.positions[tris[ti+1]],
                                   rm.positions[tris[ti+2]], t) && t<best_t) {
                    best_t=t; best_sm=si;
                }
            }
        }

        if (best_sm >= 0) {
            m_ui_state.sel_submesh = best_sm;
            m_renderer.sel_submesh = best_sm;
            
            if (m_gpu_model && best_sm < (int)m_gpu_model->meshes.size()) {
                m_ui_state.preview_tex_id   = m_gpu_model->meshes[best_sm].tex_id;
                m_ui_state.preview_tex_name = best_sm < (int)m_ui_state.submeshes.size()
                    ? m_ui_state.submeshes[best_sm].tex_candidates.empty()
                        ? m_ui_state.submeshes[best_sm].mat_name
                        : m_ui_state.submeshes[best_sm].tex_candidates[0]
                    : "";
            }
            return;
        }
    }

    
    int b = pick_bone(mx,my);
    m_sel_bone = b;
    m_renderer.sel_bone = b;
}



void App::begin_rotate(double mx) {
    if (!m_gpu_model) return;
    m_rot_mode    = true;
    m_rot_start_x = mx;
    m_rot_accum   = 0.f;
    m_rot_axis    = {0,1,0};
    if (m_sel_bone >= 0 && m_has_bones) m_cur_pose_backup = m_cur_pose;
}

void App::update_rotate(double mx) {
    if (!m_rot_mode) return;
    float angle = (float)(mx - m_rot_start_x) / (float)m_w * glm::two_pi<float>();
    float delta = angle - m_rot_accum;
    m_rot_accum  = angle;
    glm::mat4 R  = glm::rotate(glm::mat4(1), delta, m_rot_axis);

    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        glm::vec3 pivot = glm::vec3(m_cur_pose[m_sel_bone][3]);
        int nb = (int)m_skeleton->bones.size();
        std::vector<bool> aff(nb, false);
        aff[m_sel_bone] = true;
        for (int i = 0; i < nb; ++i)
            if (m_skeleton->bones[i].parent >= 0 && aff[m_skeleton->bones[i].parent])
                aff[i] = true;

        for (int i = 0; i < nb; ++i) {
            if (!aff[i]) continue;
            glm::mat4& M = m_cur_pose[i];
            glm::vec3 t  = glm::vec3(M[3]);
            M    = R * M;
            M[3] = glm::vec4(glm::vec3(R * glm::vec4(t - pivot, 0.f)) + pivot, 1.f);
        }
        for (int i = 0; i < nb; ++i)
            if (aff[i]) m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);

        upload_skinning();
        rebuild_skel_gpu();
    } else {
        m_model_rot_y += delta;
        m_renderer.model_rot_y = m_model_rot_y;
    }
}

void App::confirm_rotate() { m_rot_mode = false; }

void App::cancel_rotate() {
    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        m_cur_pose = m_cur_pose_backup;
        for (int i = 0; i < (int)m_skeleton->bones.size(); ++i)
            m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);
        upload_skinning();
        rebuild_skel_gpu();
    } else {
        m_model_rot_y -= m_rot_accum;
        m_renderer.model_rot_y = m_model_rot_y;
    }
    m_rot_mode = false;
}

void App::rebuild_skel_gpu() {
    if (m_gpu_skel && m_skeleton) m_gpu_skel->build(*m_skeleton);
}



void App::cb_key(GLFWwindow* w, int key, int, int action, int) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) {
            if (g_app->m_rot_mode) {
                g_app->cancel_rotate();
            } else {
                g_app->m_ui_state.sel_submesh = -1;
                g_app->m_renderer.sel_submesh = -1;
                g_app->m_ui_state.preview_tex_id = 0;
                g_app->m_ui_state.preview_tex_name.clear();
                g_app->m_sel_bone = -1;
                g_app->m_renderer.sel_bone = -1;
            }
        }
        if (key == GLFW_KEY_R && !g_app->m_rot_mode) {
            double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
            g_app->begin_rotate(mx);
        }
        if (g_app->m_rot_mode) {
            if (key==GLFW_KEY_X) g_app->m_rot_axis={1,0,0};
            if (key==GLFW_KEY_Y) g_app->m_rot_axis={0,1,0};
            if (key==GLFW_KEY_Z) g_app->m_rot_axis={0,0,1};
        }
        
        if (key == GLFW_KEY_SPACE) {
            if (g_app->m_anim_sel >= 0) {
                if (!g_app->m_anim_play &&
                    g_app->m_anim_sel < (int)g_app->m_anim_clips.size()) {
                    const AnimClip& clip = g_app->m_anim_clips[g_app->m_anim_sel];
                    float dur = clip.duration > 0 ? clip.duration
                              : (clip.fps > 0 && clip.frame_count > 1
                                 ? (float)(clip.frame_count - 1) / clip.fps : 1.f);
                    if (g_app->m_anim_time >= dur) g_app->m_anim_time = 0.f;
                }
                g_app->m_anim_play          = !g_app->m_anim_play;
                g_app->m_ui_state.anim_playing = g_app->m_anim_play;
                g_app->m_ui_state.anim_time = g_app->m_anim_time;
                g_app->m_last_frame         = glfwGetTime();
                if (!g_app->m_anim_play) {
                    
                    g_app->apply_animation_pose(g_app->m_anim_time);
                }
            }
        }
    }
    if ((action==GLFW_PRESS||action==GLFW_REPEAT) && key==GLFW_KEY_ENTER && g_app->m_rot_mode)
        g_app->confirm_rotate();
}

void App::cb_mouse_btn(GLFWwindow*, int btn, int action, int) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
    bool in_vp = mx >= vp_x();

    if (action == GLFW_PRESS) {
        if (btn == GLFW_MOUSE_BUTTON_LEFT) {
            if (g_app->m_rot_mode) g_app->confirm_rotate();
            else if (in_vp) {
                if (!g_app->m_world_mode) g_app->try_select(mx,my);
                g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
            }
        }
        if (btn == GLFW_MOUSE_BUTTON_RIGHT && in_vp) {
            if (g_app->m_rot_mode) { g_app->cancel_rotate(); }
            else if (g_app->m_world_mode && g_app->m_cam.fly) {
                
                g_app->m_fly_look = true;
                g_app->m_lx = mx; g_app->m_ly = my;
                glfwSetInputMode(g_app->m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            } else {
                g_app->m_drag_r=true; g_app->m_lx=mx; g_app->m_ly=my;
            }
        }
        if (btn == GLFW_MOUSE_BUTTON_MIDDLE && in_vp) {
            g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
        }
    } else {
        if (btn==GLFW_MOUSE_BUTTON_LEFT||btn==GLFW_MOUSE_BUTTON_MIDDLE) g_app->m_drag_l=false;
        if (btn==GLFW_MOUSE_BUTTON_RIGHT) {
            g_app->m_drag_r=false;
            if (g_app->m_fly_look) {
                g_app->m_fly_look = false;
                glfwSetInputMode(g_app->m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }
}

void App::cb_cursor(GLFWwindow*, double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double dx=mx-g_app->m_lx, dy=my-g_app->m_ly;
    g_app->m_lx=mx; g_app->m_ly=my;
    if (g_app->m_rot_mode) { g_app->update_rotate(mx); return; }
    if (g_app->m_fly_look) { g_app->m_cam.fly_look((float)dx,(float)dy); return; }
    if (g_app->m_drag_l)   g_app->m_cam.orbit((float)dx,(float)dy);
    if (g_app->m_drag_r)   g_app->m_cam.do_pan((float)dx,(float)dy);
}

void App::cb_scroll(GLFWwindow*, double, double dy) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
    if (mx>=vp_x()) g_app->m_cam.zoom((float)dy);
}

void App::cb_resize(GLFWwindow*, int w, int h) {
    g_app->m_w=w; g_app->m_h=h; glViewport(0,0,w,h);
}
