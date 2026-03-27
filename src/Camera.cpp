#include "Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

void Camera::reset() {
    fly   = false;
    yaw   = 30.f;
    pitch = -20.f;
    dist  = 2.5f;
    pan   = {0,0,0};
}

void Camera::reset_fly(glm::vec3 pos) {
    fly       = true;
    fly_pos   = pos;
    yaw       = 0.f;
    pitch     = -15.f;
}

glm::vec3 Camera::eye() const {
    if (fly) return fly_pos;
    float yr = glm::radians(yaw);
    float pr = glm::radians(pitch);
    return glm::vec3(
        dist * cosf(pr) * sinf(yr),
        dist * sinf(pr),
        dist * cosf(pr) * cosf(yr)
    ) + pan;
}

glm::mat4 Camera::view() const {
    if (fly) {
        float yr = glm::radians(yaw);
        float pr = glm::radians(pitch);
        glm::vec3 fwd = glm::normalize(glm::vec3(
            cosf(pr) * sinf(yr),
            sinf(pr),
            cosf(pr) * cosf(yr)
        ));
        return glm::lookAt(fly_pos, fly_pos + fwd, glm::vec3(0,1,0));
    }
    return glm::lookAt(eye(), pan, glm::vec3(0,1,0));
}

glm::mat4 Camera::proj(float aspect) const {
    float near_p = fly ? 1.0f   : 0.01f;
    float far_p  = fly ? 8000.f : 1000.f;
    return glm::perspective(glm::radians(60.f), aspect, near_p, far_p);
}

glm::mat4 Camera::mvp(float aspect, glm::vec3 center, float scale) const {
    glm::mat4 S = glm::scale(glm::mat4(1), glm::vec3(1.f/scale));
    glm::mat4 T = glm::translate(glm::mat4(1), -center);
    return proj(aspect) * view() * S * T;
}

glm::vec3 Camera::ray_dir(float mx, float my,
                           int vp_x, int vp_w, int vp_h) const {
    float nx = ((mx - vp_x) / vp_w) * 2.f - 1.f;
    float ny = 1.f - (my / vp_h) * 2.f;
    float aspect = (float)vp_w / (float)vp_h;

    glm::vec4 clip  = {nx, ny, -1.f, 1.f};
    glm::vec4 eye_  = glm::inverse(proj(aspect)) * clip;
    eye_             = {eye_.x, eye_.y, -1.f, 0.f};
    glm::vec3 world = glm::normalize(glm::vec3(glm::inverse(view()) * eye_));
    return world;
}

void Camera::orbit(float dx, float dy) {
    yaw   += dx * 0.5f;
    pitch  = glm::clamp(pitch - dy * 0.5f, -89.f, 89.f);
}

void Camera::do_pan(float dx, float dy) {
    float yr = glm::radians(yaw);
    glm::vec3 right = { cosf(yr), 0.f, -sinf(yr) };
    float spd = dist * 0.002f;
    pan -= right * dx * spd;
    pan += glm::vec3(0,1,0) * dy * spd;
}

void Camera::zoom(float delta) {
    if (fly) {
        fly_speed = glm::clamp(fly_speed * (delta > 0 ? 0.8f : 1.25f), 1.f, 5000.f);
        return;
    }
    dist = glm::clamp(dist * (delta > 0 ? 0.9f : 1.1f), 0.05f, 500.f);
}

void Camera::fly_move(float dt, unsigned keys) {
    if (!fly) return;
    float yr = glm::radians(yaw);
    float pr = glm::radians(pitch);
    glm::vec3 fwd = glm::normalize(glm::vec3(
        cosf(pr) * sinf(yr),
        sinf(pr),
        cosf(pr) * cosf(yr)
    ));
    glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0,1,0)));
    glm::vec3 up    = {0, 1, 0};

    float spd = fly_speed * dt;
    if (keys & 1)  fly_pos += fwd   * spd;
    if (keys & 2)  fly_pos -= fwd   * spd;
    if (keys & 4)  fly_pos -= right * spd;
    if (keys & 8)  fly_pos += right * spd;
    if (keys & 16) fly_pos -= up    * spd;
    if (keys & 32) fly_pos += up    * spd;
}

void Camera::fly_look(float dx, float dy) {
    if (!fly) return;
    yaw   += dx * 0.15f;
    pitch  = glm::clamp(pitch - dy * 0.15f, -89.f, 89.f);
}