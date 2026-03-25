#pragma once
#include <glm/glm.hpp>

class Camera {
public:
    float yaw   = 30.f;
    float pitch = -20.f;
    float dist  = 2.5f;
    glm::vec3 pan{0,0,0};

    void reset();

    glm::mat4 view()            const;
    glm::mat4 proj(float aspect) const;
    glm::mat4 mvp(float aspect, glm::vec3 center, float scale) const;

    // Mouse interaction helpers
    void orbit(float dx, float dy);
    void do_pan(float dx, float dy);
    void zoom(float delta);
};
