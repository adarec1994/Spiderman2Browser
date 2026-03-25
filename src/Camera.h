#pragma once
#include <glm/glm.hpp>

class Camera {
public:
    float yaw   = 30.f;
    float pitch = -20.f;
    float dist  = 2.5f;
    glm::vec3 pan{0,0,0};

    void reset();

    glm::mat4 view()             const;
    glm::mat4 proj(float aspect) const;
    glm::mat4 mvp(float aspect, glm::vec3 center, float scale) const;

    glm::vec3 eye()  const;
    // Unproject screen pixel to world-space ray direction.
    // vp_x/vp_w/vp_h: viewport x-offset, width, height (px). mx/my: screen px.
    glm::vec3 ray_dir(float mx, float my,
                      int vp_x, int vp_w, int vp_h) const;

    void orbit(float dx, float dy);
    void do_pan(float dx, float dy);
    void zoom(float delta);
};