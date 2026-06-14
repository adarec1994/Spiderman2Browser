#pragma once
#include <glm/glm.hpp>

class Camera {
public:
    float yaw   = 30.f;
    float pitch = -20.f;
    float dist  = 2.5f;
    glm::vec3 pan{0,0,0};

    
    bool      fly      = false;   
    glm::vec3 fly_pos  {0,0,0};   
    float     fly_speed = 50.f;   

    void reset();
    void reset_fly(glm::vec3 pos); 

    glm::mat4 view()             const;
    glm::mat4 proj(float aspect) const;
    glm::mat4 mvp(float aspect, glm::vec3 center, float scale) const;

    glm::vec3 eye()  const;
    glm::vec3 ray_dir(float mx, float my,
                      int vp_x, int vp_w, int vp_h) const;

    void orbit(float dx, float dy);
    void do_pan(float dx, float dy);
    void zoom(float delta);

    
    
    void fly_move(float dt, unsigned keys);
    
    void fly_look(float dx, float dy);
};