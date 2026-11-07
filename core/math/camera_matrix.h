/**
 * @file camera_matrix.h
 * @brief CameraMatrix - 3D projection + view matrix computation (ex-Camera3D)
 *
 * Pure math component, no GPU resources. Moved from scene/3d/ to core/math/
 * to align with Godot's core/math/camera_matrix.h placement.
 *
 * @author yameng.he
 * @version 2.0
 * @date 2026-08-22
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#ifndef ARHUD_CORE_MATH_CAMERA_MATRIX_H_
#define ARHUD_CORE_MATH_CAMERA_MATRIX_H_

#include "math/glm_types.h"
#include "math/math_funcs.h"

namespace arhud {
enum class ProjectionType : uint8_t {
  kPerspective,
  kOrthographic,
};

enum class KeepAspect : uint8_t {
  kKeepHeight,
  kKeepWidth,
};

class CameraMatrix {
 public:
  CameraMatrix() = default;

  void SetPerspective(float32 p_fov_y, float32 p_aspect, float32 p_near_z,
                      float32 p_far_z);
  void SetOrthographic(float32 p_ortho_size, float32 p_aspect, float32 p_near_z,
                       float32 p_far_z);
  void SetOrthographic(float32 p_left, float32 p_right, float32 p_bottom,
                       float32 p_top, float32 p_near_z, float32 p_far_z);

  void LookAt(const Vec3& p_eye, const Vec3& p_target,
              const Vec3& p_up = Vec3::kUp);
  void SetViewTransform(const Mat4& p_view_matrix);

  void SetPosition(const Vec3& p_position);
  void SetTarget(const Vec3& p_target);
  void SetUp(const Vec3& p_up);
  void SetAspect(float32 p_aspect);
  void SetKeepAspect(KeepAspect p_keep_aspect);

  void UpdateMatrices() const;

  ProjectionType GetProjectionType() const {
    return projection_type_;
  }
  KeepAspect GetKeepAspect() const {
    return keep_aspect_;
  }
  float32 GetFOV() const {
    return fov_y_;
  }
  float32 GetAspect() const {
    return aspect_;
  }
  float32 GetNearZ() const {
    return near_z_;
  }
  float32 GetFarZ() const {
    return far_z_;
  }
  float32 GetOrthoSize() const {
    return ortho_size_;
  }
  /// @brief 是否处于"手工视图矩阵"模式（SetViewTransform；命令链不支持，见
  /// ADR-0006）
  bool IsUsingManualView() const {
    return use_manual_view_;
  }
  /// @brief
  /// 是否处于"手工正交矩形"模式（SetOrthographic(left,right,bottom,top,...)）
  bool IsUsingManualOrthoRect() const {
    return use_manual_ortho_rect_;
  }
  /// @brief 取正交手工矩形（仅 IsUsingManualOrthoRect() 为真时有意义）
  void GetOrthoRect(float32& r_left, float32& r_right, float32& r_bottom,
                    float32& r_top) const {
    r_left = ortho_left_;
    r_right = ortho_right_;
    r_bottom = ortho_bottom_;
    r_top = ortho_top_;
  }
  const Vec3& GetPosition() const {
    return position_;
  }
  const Vec3& GetTarget() const {
    return target_;
  }
  const Vec3& GetUp() const {
    return up_;
  }

  const float32* GetViewMatrix() const {
    return view_matrix_.elements;
  }
  const float32* GetProjectionMatrix() const {
    return projection_matrix_.elements;
  }
  const float32* GetViewProjectionMatrix() const {
    return view_projection_matrix_.elements;
  }
  const Mat4& GetViewMat4() const {
    return view_matrix_;
  }
  const Mat4& GetProjectionMat4() const {
    return projection_matrix_;
  }
  const Mat4& GetViewProjectionMat4() const {
    return view_projection_matrix_;
  }
  bool IsDirty() const {
    return matrices_dirty_;
  }

 private:
  ProjectionType projection_type_ = ProjectionType::kPerspective;
  KeepAspect keep_aspect_ = KeepAspect::kKeepHeight;

  float32 fov_y_ = 45.0f;
  float32 aspect_ = 16.0f / 9.0f;
  float32 near_z_ = 0.1f;
  float32 far_z_ = 100.0f;

  float32 ortho_size_ = 1.0f;
  float32 ortho_left_ = 0.0f;
  float32 ortho_right_ = 0.0f;
  float32 ortho_bottom_ = 0.0f;
  float32 ortho_top_ = 0.0f;
  bool use_manual_ortho_rect_ = false;

  Vec3 position_ = {0.0f, 0.0f, 0.0f};
  Vec3 target_ = {0.0f, 0.0f, -1.0f};
  Vec3 up_ = Vec3::kUp;

  Mat4 manual_view_matrix_;
  bool use_manual_view_ = false;

  mutable Mat4 view_matrix_;
  mutable Mat4 projection_matrix_;
  mutable Mat4 view_projection_matrix_;
  mutable bool matrices_dirty_ = true;

  static constexpr float32 kDegToRad = 0.017453292519943295f;
};

}  // namespace arhud

#endif  // ARHUD_CORE_MATH_CAMERA_MATRIX_H_
