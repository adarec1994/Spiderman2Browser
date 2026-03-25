#include "Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

void Camera::reset() {
    yaw   = 30.f;
    pitch = -20.f;
    dist  = 2.5f;
    pan   = {0,0,0};
}

glm::mat4 Camera::view() const {
    float yr = glm::radians(yaw);
    float pr = glm::radians(pitch);
    glm::vec3 eye = glm::vec3(
        dist * cosf(pr) * sinf(yr),
        dist * sinf(pr),
        dist * cosf(pr) * cosf(yr)
    ) + pan;
    return glm::lookAt(eye, pan, glm::vec3(0,1,0));
}

glm::mat4 Camera::proj(float aspect) const {
    return glm::perspective(glm::radians(60.f), aspect, 0.01f, 1000.f);
}

glm::mat4 Camera::mvp(float aspect, glm::vec3 center, float scale) const {
    glm::mat4 S = glm::scale(glm::mat4(1), glm::vec3(1.f/scale));
    glm::mat4 T = glm::translate(glm::mat4(1), -center);
    return proj(aspect) * view() * S * T;
}

void Camera::orbit(float dx, float dy) {
    yaw   += dx * 0.5f;
    pitch  = glm::clamp(pitch - dy * 0.5f, -89.f, 89.f);
}

void Camera::do_pan(float dx, float dy) {
    float yr = glm::radians(yaw);
    glm::vec3 right = { cosf(yr), 0.f, -sinf(yr) };
    glm::vec3 up    = { 0.f, 1.f, 0.f };
    float spd = dist * 0.002f;
    pan -= right * dx * spd;
    pan += up    * dy * spd;
}

void Camera::zoom(float delta) {
    dist = glm::clamp(dist * (delta > 0 ? 0.9f : 1.1f), 0.05f, 500.f);
}
