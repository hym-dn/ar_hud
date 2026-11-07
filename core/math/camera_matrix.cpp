/**
 * @file camera_matrix.cpp
 * @brief CameraMatrix implementation (ex-Camera3D)
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-08-22
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include "camera_matrix.h"

namespace arhud {
void CameraMatrix::SetPerspective(float32 p_fov_y, float32 p_aspect,
                                  float32 p_near_z, float32 p_far_z) {
  projection_type_ = ProjectionType::kPerspective;
  fov_y_ = p_fov_y;
  aspect_ = p_aspect;
  near_z_ = p_near_z;
  far_z_ = p_far_z;
  matrices_dirty_ = true;
}

void CameraMatrix::SetOrthographic(float32 p_ortho_size, float32 p_aspect,
                                   float32 p_near_z, float32 p_far_z) {
  projection_type_ = ProjectionType::kOrthographic;
  ortho_size_ = p_ortho_size;
  aspect_ = p_aspect;
  near_z_ = p_near_z;
  far_z_ = p_far_z;
  use_manual_ortho_rect_ = false;
  matrices_dirty_ = true;
}

void CameraMatrix::SetOrthographic(float32 p_left, float32 p_right,
                                   float32 p_bottom, float32 p_top,
                                   float32 p_near_z, float32 p_far_z) {
  projection_type_ = ProjectionType::kOrthographic;
  ortho_left_ = p_left;
  ortho_right_ = p_right;
  ortho_bottom_ = p_bottom;
  ortho_top_ = p_top;
  near_z_ = p_near_z;
  far_z_ = p_far_z;
  use_manual_ortho_rect_ = true;
  matrices_dirty_ = true;
}

void CameraMatrix::LookAt(const Vec3& p_eye, const Vec3& p_target,
                          const Vec3& p_up) {
  position_ = p_eye;
  target_ = p_target;
  up_ = p_up;
  use_manual_view_ = false;
  matrices_dirty_ = true;
}

void CameraMatrix::SetViewTransform(const Mat4& p_view_matrix) {
  manual_view_matrix_ = p_view_matrix;
  use_manual_view_ = true;
  matrices_dirty_ = true;
}

void CameraMatrix::SetPosition(const Vec3& p_position) {
  position_ = p_position;
  if (!use_manual_view_) {
    matrices_dirty_ = true;
  }
}

void CameraMatrix::SetTarget(const Vec3& p_target) {
  target_ = p_target;
  if (!use_manual_view_) {
    matrices_dirty_ = true;
  }
}

void CameraMatrix::SetUp(const Vec3& p_up) {
  up_ = p_up;
  if (!use_manual_view_) {
    matrices_dirty_ = true;
  }
}

void CameraMatrix::SetAspect(float32 p_aspect) {
  aspect_ = p_aspect;
  matrices_dirty_ = true;
}

void CameraMatrix::SetKeepAspect(KeepAspect p_keep_aspect) {
  keep_aspect_ = p_keep_aspect;
  matrices_dirty_ = true;
}

void CameraMatrix::UpdateMatrices() const {
  if (!matrices_dirty_) {
    return;
  }

  // View matrix
  if (use_manual_view_) {
    view_matrix_ = manual_view_matrix_;
  } else {
    view_matrix_ = Mat4::LookAt(position_, target_, up_);
  }

  // Projection matrix
  if (projection_type_ == ProjectionType::kPerspective) {
    float32 effective_fov_y = fov_y_;
    if (keep_aspect_ == KeepAspect::kKeepWidth) {
      float32 fov_x = fov_y_ * aspect_;
      effective_fov_y =
          2.0f * atan(tan(fov_x * 0.5f * kDegToRad) / aspect_) / kDegToRad;
    }
    projection_matrix_ =
        Perspective(effective_fov_y * kDegToRad, aspect_, near_z_, far_z_);
  } else if (use_manual_ortho_rect_) {
    projection_matrix_ = Orthographic(ortho_left_, ortho_right_, ortho_bottom_,
                                      ortho_top_, near_z_, far_z_);
  } else {
    float32 half_h = ortho_size_;
    float32 half_w = ortho_size_ * aspect_;
    projection_matrix_ =
        Orthographic(-half_w, half_w, -half_h, half_h, near_z_, far_z_);
  }

  view_projection_matrix_ = projection_matrix_ * view_matrix_;
  matrices_dirty_ = false;
}

}  // namespace arhud
