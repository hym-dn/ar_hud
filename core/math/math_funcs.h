/**
 * @file arhud_math.h
 * @brief 数学库 — 向量、矩阵、颜色、变换、数学工具函数
 *
 * 参考 Godot 4.6 core/math/ 设计，为 AR HUD 2D 渲染场景定制。
 * 所有类型均为 header-only 实现，使用 ARHUD_ALWAYS_INLINE 保证零开销抽象。
 *
 * 类型清单：
 *   MathFuncs    — 三角/插值/钳位/常量
 *   Vec2         — 2D 浮点向量
 *   Vec2i        — 2D 整数向量
 *   Vec3         — 3D 浮点向量
 *   Vec4         — 4D 浮点向量
 *   Size2i       — 2D 整数尺寸
 *   Rect2        — 2D 浮点矩形
 *   Rect2i       — 2D 整数矩形
 *   Color        — RGBA 浮点颜色
 *   Transform2D  — 2D 仿射变换（2×3 列主序）
 *   Mat4         — 4×4 列主序矩阵
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include "typedefs.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // MathFuncs — 数学工具函数与常量
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 数学工具函数与常量命名空间
     *
     * 提供 GPU 渲染引擎所需的数学原语：三角函数、插值、钳位、角度转换等。
     * 所有函数均为 ARHUD_ALWAYS_INLINE，在 Release 构建中零开销。
     */
    namespace MathFuncs
    {

        /** @brief 圆周率 π = 3.14159265... */
        inline constexpr float32 kPi = 3.14159265358979323846f;
        /** @brief 圆周率 2π = 6.28318530... */
        inline constexpr float32 kTau = 6.28318530717958647692f;
        /** @brief 默认浮点比较精度阈值 */
        inline constexpr float32 kEpsilon = 0.00001f;
        /** @brief kEpsilon 的平方，用于平方级比较 */
        inline constexpr float32 kEpsilon2 = kEpsilon * kEpsilon;
        /** @brief √2 ≈ 1.41421356 */
        inline constexpr float32 kSqrt2 = 1.41421356237309504880f;
        /** @brief √3 ≈ 1.73205080 */
        inline constexpr float32 kSqrt3 = 1.73205080756887729352f;
        /** @brief 正无穷 */
        inline constexpr float32 kInf = std::numeric_limits<float32>::infinity();
        /** @brief 安静 NaN（非数字） */
        inline constexpr float32 kNaN = std::numeric_limits<float32>::quiet_NaN();

        /** @brief 正弦
         *  @param[in] p_x 弧度角
         *  @return sin(p_x) */
        ARHUD_ALWAYS_INLINE float32 Sin(float32 p_x) { return std::sin(p_x); }
        /** @brief 余弦
         *  @param[in] p_x 弧度角
         *  @return cos(p_x) */
        ARHUD_ALWAYS_INLINE float32 Cos(float32 p_x) { return std::cos(p_x); }
        /** @brief 正切
         *  @param[in] p_x 弧度角
         *  @return tan(p_x) */
        ARHUD_ALWAYS_INLINE float32 Tan(float32 p_x) { return std::tan(p_x); }
        /** @brief 双曲正弦
         *  @param[in] p_x 弧度角
         *  @return sinh(p_x) */
        ARHUD_ALWAYS_INLINE float32 Sinh(float32 p_x) { return std::sinh(p_x); }
        /** @brief 双曲余弦
         *  @param[in] p_x 弧度角
         *  @return cosh(p_x) */
        ARHUD_ALWAYS_INLINE float32 Cosh(float32 p_x) { return std::cosh(p_x); }
        /** @brief 双曲正切
         *  @param[in] p_x 弧度角
         *  @return tanh(p_x) */
        ARHUD_ALWAYS_INLINE float32 Tanh(float32 p_x) { return std::tanh(p_x); }

        /** @brief 反正弦（带边界钳位）
         *  @param[in] p_x [-1, 1] 区间值
         *  @return asin(p_x)，越界时返回 ±π/2 */
        ARHUD_ALWAYS_INLINE float32 Asin(float32 p_x)
        {
            return p_x < -1.0f ? (-kPi / 2.0f) : (p_x > 1.0f ? (kPi / 2.0f) : std::asin(p_x));
        }

        /** @brief 反余弦（带边界钳位）
         *  @param[in] p_x [-1, 1] 区间值
         *  @return acos(p_x)，越界时返回 0 或 π */
        ARHUD_ALWAYS_INLINE float32 Acos(float32 p_x)
        {
            return p_x < -1.0f ? kPi : (p_x > 1.0f ? 0.0f : std::acos(p_x));
        }

        /** @brief 反正切
         *  @param[in] p_x 实数
         *  @return atan(p_x)，范围 [-π/2, π/2] */
        ARHUD_ALWAYS_INLINE float32 Atan(float32 p_x) { return std::atan(p_x); }
        /** @brief 四象限反正切
         *  @param[in] p_y y 分量
         *  @param[in] p_x x 分量
         *  @return atan2(p_y, p_x)，范围 [-π, π] */
        ARHUD_ALWAYS_INLINE float32 Atan2(float32 p_y, float32 p_x) { return std::atan2(p_y, p_x); }
        /** @brief 平方根
         *  @param[in] p_x 非负实数
         *  @return √p_x */
        ARHUD_ALWAYS_INLINE float32 Sqrt(float32 p_x) { return std::sqrt(p_x); }
        /** @brief 浮点取模（余数）
         *  @param[in] p_x 被除数
         *  @param[in] p_y 除数
         *  @return fmod(p_x, p_y) */
        ARHUD_ALWAYS_INLINE float32 Fmod(float32 p_x, float32 p_y) { return std::fmod(p_x, p_y); }
        /** @brief 向下取整
         *  @param[in] p_x 浮点数
         *  @return ⌊p_x⌋ */
        ARHUD_ALWAYS_INLINE float32 Floor(float32 p_x) { return std::floor(p_x); }
        /** @brief 向上取整
         *  @param[in] p_x 浮点数
         *  @return ⌈p_x⌉ */
        ARHUD_ALWAYS_INLINE float32 Ceil(float32 p_x) { return std::ceil(p_x); }
        /** @brief 四舍五入
         *  @param[in] p_x 浮点数
         *  @return 最接近的整数 */
        ARHUD_ALWAYS_INLINE float32 Round(float32 p_x) { return std::round(p_x); }
        /** @brief 幂运算
         *  @param[in] p_x 底数
         *  @param[in] p_y 指数
         *  @return p_x^p_y */
        ARHUD_ALWAYS_INLINE float32 Pow(float32 p_x, float32 p_y) { return std::pow(p_x, p_y); }
        /** @brief 自然对数
         *  @param[in] p_x 正实数
         *  @return ln(p_x) */
        ARHUD_ALWAYS_INLINE float32 Log(float32 p_x) { return std::log(p_x); }
        /** @brief 以 2 为底的对数
         *  @param[in] p_x 正实数
         *  @return log₂(p_x) */
        ARHUD_ALWAYS_INLINE float32 Log2(float32 p_x) { return std::log2(p_x); }
        /** @brief 指数函数
         *  @param[in] p_x 实数
         *  @return e^p_x */
        ARHUD_ALWAYS_INLINE float32 Exp(float32 p_x) { return std::exp(p_x); }
        /** @brief 浮点绝对值
         *  @param[in] p_x 浮点数
         *  @return |p_x| */
        ARHUD_ALWAYS_INLINE float32 Abs(float32 p_x) { return std::abs(p_x); }
        /** @brief 整数绝对值
         *  @param[in] p_x 整数
         *  @return |p_x| */
        ARHUD_ALWAYS_INLINE int32 Abs(int32 p_x) { return std::abs(p_x); }
        /** @brief 判断是否为 NaN
         *  @param[in] p_x 浮点数
         *  @return true 当 p_x 是 NaN */
        ARHUD_ALWAYS_INLINE bool IsNaN(float32 p_x) { return std::isnan(p_x); }
        /** @brief 判断是否为无穷
         *  @param[in] p_x 浮点数
         *  @return true 当 p_x 是 ±∞ */
        ARHUD_ALWAYS_INLINE bool IsInf(float32 p_x) { return std::isinf(p_x); }
        /** @brief 判断是否有限
         *  @param[in] p_x 浮点数
         *  @return true 当 p_x 是有限值 */
        ARHUD_ALWAYS_INLINE bool IsFinite(float32 p_x) { return std::isfinite(p_x); }
        /** @brief 角度转弧度
         *  @param[in] p_deg 角度值
         *  @return 弧度值 */
        ARHUD_ALWAYS_INLINE float32 DegToRad(float32 p_deg) { return p_deg * (kPi / 180.0f); }
        /** @brief 弧度转角度
         *  @param[in] p_rad 弧度值
         *  @return 角度值 */
        ARHUD_ALWAYS_INLINE float32 RadToDeg(float32 p_rad) { return p_rad * (180.0f / kPi); }

        /** @brief 浮点最小值
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @return 较小的值 */
        ARHUD_ALWAYS_INLINE float32 Min(float32 p_a, float32 p_b) { return p_a < p_b ? p_a : p_b; }
        /** @brief 浮点最大值
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @return 较大的值 */
        ARHUD_ALWAYS_INLINE float32 Max(float32 p_a, float32 p_b) { return p_a > p_b ? p_a : p_b; }
        /** @brief 整数最小值
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @return 较小的值 */
        ARHUD_ALWAYS_INLINE int32 Min(int32 p_a, int32 p_b) { return p_a < p_b ? p_a : p_b; }
        /** @brief 整数最大值
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @return 较大的值 */
        ARHUD_ALWAYS_INLINE int32 Max(int32 p_a, int32 p_b) { return p_a > p_b ? p_a : p_b; }
        /** @brief 无符号整数最小值
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @return 较小的值 */
        ARHUD_ALWAYS_INLINE uint32 Min(uint32 p_a, uint32 p_b) { return p_a < p_b ? p_a : p_b; }
        /** @brief 无符号整数最大值
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @return 较大的值 */
        ARHUD_ALWAYS_INLINE uint32 Max(uint32 p_a, uint32 p_b) { return p_a > p_b ? p_a : p_b; }

        /** @brief 浮点钳位
         *  @param[in] p_val 待钳位值
         *  @param[in] p_min 下限
         *  @param[in] p_max 上限
         *  @return clamp(p_val, p_min, p_max) */
        ARHUD_ALWAYS_INLINE float32 Clamp(float32 p_val, float32 p_min, float32 p_max)
        {
            return p_val < p_min ? p_min : (p_val > p_max ? p_max : p_val);
        }

        /** @brief 整数钳位
         *  @param[in] p_val 待钳位值
         *  @param[in] p_min 下限
         *  @param[in] p_max 上限
         *  @return clamp(p_val, p_min, p_max) */
        ARHUD_ALWAYS_INLINE int32 Clamp(int32 p_val, int32 p_min, int32 p_max)
        {
            return p_val < p_min ? p_min : (p_val > p_max ? p_max : p_val);
        }

        /** @brief 近似相等比较（相对误差自适应）
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @return |a-b| < max(kEpsilon, kEpsilon*|a|) */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(float32 p_a, float32 p_b)
        {
            if (p_a == p_b)
            {
                return true;
            }
            float32 tolerance = kEpsilon * Abs(p_a);
            if (tolerance < kEpsilon)
            {
                tolerance = kEpsilon;
            }
            return Abs(p_a - p_b) < tolerance;
        }

        /** @brief 近似相等比较（指定容差）
         *  @param[in] p_a 比较值 a
         *  @param[in] p_b 比较值 b
         *  @param[in] p_tolerance 绝对容差
         *  @return |a-b| < p_tolerance */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(float32 p_a, float32 p_b, float32 p_tolerance)
        {
            if (p_a == p_b)
            {
                return true;
            }
            return Abs(p_a - p_b) < p_tolerance;
        }

        /** @brief 近似零值判断
         *  @param[in] p_val 待判断值
         *  @return |p_val| < kEpsilon */
        ARHUD_ALWAYS_INLINE bool IsZeroApprox(float32 p_val) { return Abs(p_val) < kEpsilon; }

        /** @brief 浮点符号函数
         *  @param[in] p_x 浮点数
         *  @return 1.0(p_x>0) / -1.0(p_x<0) / 0.0(p_x==0) */
        ARHUD_ALWAYS_INLINE float32 Sign(float32 p_x)
        {
            return p_x > 0.0f ? 1.0f : (p_x < 0.0f ? -1.0f : 0.0f);
        }

        /** @brief 整数符号函数
         *  @param[in] p_x 整数
         *  @return 1(p_x>0) / -1(p_x<0) / 0(p_x==0) */
        ARHUD_ALWAYS_INLINE int32 Sign(int32 p_x) { return p_x > 0 ? 1 : (p_x < 0 ? -1 : 0); }

        /** @brief 线性插值
         *  @param[in] p_from 起点值
         *  @param[in] p_to 终点值
         *  @param[in] p_weight 插值权重 [0, 1]
         *  @return p_from + (p_to - p_from) * p_weight */
        ARHUD_ALWAYS_INLINE float32 Lerp(float32 p_from, float32 p_to, float32 p_weight)
        {
            return p_from + (p_to - p_from) * p_weight;
        }

        /** @brief 反线性插值（计算归一化位置）
         *  @param[in] p_from 区间起点
         *  @param[in] p_to 区间终点
         *  @param[in] p_value 区间内的值
         *  @return p_value 在 [p_from, p_to] 中的归一化位置 */
        ARHUD_ALWAYS_INLINE float32 InverseLerp(float32 p_from, float32 p_to, float32 p_value)
        {
            return (p_value - p_from) / (p_to - p_from);
        }

        /** @brief 值域重映射
         *  @param[in] p_value 源区间的值
         *  @param[in] p_istart 源区间起点
         *  @param[in] p_istop 源区间终点
         *  @param[in] p_ostart 目标区间起点
         *  @param[in] p_ostop 目标区间终点
         *  @return p_value 从 [istart, istop] 映射到 [ostart, ostop] */
        ARHUD_ALWAYS_INLINE float32 Remap(float32 p_value, float32 p_istart, float32 p_istop,
                                          float32 p_ostart, float32 p_ostop)
        {
            return Lerp(p_ostart, p_ostop, InverseLerp(p_istart, p_istop, p_value));
        }

        /** @brief Hermite 平滑插值（三次 Hermite 曲线）
         *  @param[in] p_from 起点值
         *  @param[in] p_to 终点值
         *  @param[in] p_s 插值参数
         *  @return 3s² - 2s³ 平滑插值结果 */
        ARHUD_ALWAYS_INLINE float32 Smoothstep(float32 p_from, float32 p_to, float32 p_s)
        {
            if (IsEqualApprox(p_from, p_to))
            {
                return p_s <= p_from ? 0.0f : 1.0f;
            }
            float32 s = Clamp((p_s - p_from) / (p_to - p_from), 0.0f, 1.0f);
            return s * s * (3.0f - 2.0f * s);
        }

        /** @brief 以固定步长逼近目标
         *  @param[in] p_from 当前值
         *  @param[in] p_to 目标值
         *  @param[in] p_delta 最大步长
         *  @return 向目标移动 p_delta 后的值 */
        ARHUD_ALWAYS_INLINE float32 MoveToward(float32 p_from, float32 p_to, float32 p_delta)
        {
            return Abs(p_to - p_from) <= p_delta ? p_to : p_from + Sign(p_to - p_from) * p_delta;
        }

        /** @brief 带环绕的角度差
         *  @param[in] p_from 起始角度（弧度）
         *  @param[in] p_to 目标角度（弧度）
         *  @return [-π, π] 范围内的有符号角度差 */
        ARHUD_ALWAYS_INLINE float32 AngleDifference(float32 p_from, float32 p_to)
        {
            float32 difference = Fmod(p_to - p_from, kTau);
            return Fmod(2.0f * difference, kTau) - difference;
        }

        /** @brief 角度线性插值（带环绕处理）
         *  @param[in] p_from 起始角度（弧度）
         *  @param[in] p_to 目标角度（弧度）
         *  @param[in] p_weight 插值权重
         *  @return 插值后的角度（弧度） */
        ARHUD_ALWAYS_INLINE float32 LerpAngle(float32 p_from, float32 p_to, float32 p_weight)
        {
            return p_from + AngleDifference(p_from, p_to) * p_weight;
        }

        /** @brief 三次贝塞尔插值
         *  @param[in] p_start 起点
         *  @param[in] p_control_1 控制点 1
         *  @param[in] p_control_2 控制点 2
         *  @param[in] p_end 终点
         *  @param[in] p_t 参数 [0, 1]
         *  @return 贝塞尔曲线上的值 */
        ARHUD_ALWAYS_INLINE float32 BezierInterpolate(float32 p_start, float32 p_control_1,
                                                      float32 p_control_2, float32 p_end, float32 p_t)
        {
            float32 omt = 1.0f - p_t;
            float32 omt2 = omt * omt;
            float32 omt3 = omt2 * omt;
            float32 t2 = p_t * p_t;
            float32 t3 = t2 * p_t;
            return p_start * omt3 + p_control_1 * omt2 * p_t * 3.0f +
                   p_control_2 * omt * t2 * 3.0f + p_end * t3;
        }

        /** @brief 浮点正模运算（保证结果与除数同号）
         *  @param[in] p_x 被除数
         *  @param[in] p_y 除数
         *  @return 范围 [0, p_y) 的正余数 */
        ARHUD_ALWAYS_INLINE float32 Fposmod(float32 p_x, float32 p_y)
        {
            float32 value = Fmod(p_x, p_y);
            if (((value < 0.0f) && (p_y > 0.0f)) || ((value > 0.0f) && (p_y < 0.0f)))
            {
                value += p_y;
            }
            return value;
        }

        /** @brief 浮点值在区间内循环环绕
         *  @param[in] p_value 待环绕值
         *  @param[in] p_min 区间下限
         *  @param[in] p_max 区间上限
         *  @return [p_min, p_max) 范围内的环绕值 */
        ARHUD_ALWAYS_INLINE float32 Wrapf(float32 p_value, float32 p_min, float32 p_max)
        {
            float32 range = p_max - p_min;
            if (IsZeroApprox(range))
            {
                return p_min;
            }
            float32 result = p_value - (range * Floor((p_value - p_min) / range));
            if (IsEqualApprox(result, p_max))
            {
                return p_min;
            }
            return result;
        }

        /** @brief 无符号整数除法向上取整
         *  @param[in] p_num 被除数
         *  @param[in] p_den 除数
         *  @return ⌈p_num / p_den⌉ */
        ARHUD_ALWAYS_INLINE uint32 DivisionRoundUp(uint32 p_num, uint32 p_den)
        {
            return (p_num + p_den - 1) / p_den;
        }

        /** @brief 取小数部分
         *  @param[in] p_value 浮点数
         *  @return p_value - floor(p_value) */
        ARHUD_ALWAYS_INLINE float32 Fract(float32 p_value) { return p_value - Floor(p_value); }

        /** @brief 线性值转分贝
         *  @param[in] p_linear 线性比例
         *  @return 分贝值 dB = 20 * log₁₀(linear) */
        ARHUD_ALWAYS_INLINE float32 LinearToDb(float32 p_linear)
        {
            return Log(p_linear) * 8.685889638065036553f;
        }

        /** @brief 分贝转线性值
         *  @param[in] p_db 分贝值
         *  @return 线性比例 */
        ARHUD_ALWAYS_INLINE float32 DbToLinear(float32 p_db)
        {
            return Exp(p_db * 0.115129254649702284f);
        }

    } // namespace MathFuncs

    // ═══════════════════════════════════════════════════════════════════════
    // Vec2 — 2D 浮点向量
    // ═══════════════════════════════════════════════════════════════════════

    struct Vec2i;

    /**
     * @brief 2D 浮点向量
     *
     * 参考 Godot Vector2 设计，用于 HUD 坐标、尺寸、偏移等 2D 场景。
     * 使用 union 实现 x/y 与 width/height 的别名访问。
     */
    struct Vec2
    {
        /** @brief 单位左向量 (-1, 0) */
        static const Vec2 kLeft;
        /** @brief 单位右向量 (1, 0) */
        static const Vec2 kRight;
        /** @brief 单位上向量 (0, -1) */
        static const Vec2 kUp;
        /** @brief 单位下向量 (0, 1) */
        static const Vec2 kDown;
        /** @brief 零向量 (0, 0) */
        static const Vec2 kZero;
        /** @brief 单位向量 (1, 1) */
        static const Vec2 kOne;

        /** @brief 轴枚举 */
        enum class Axis : int32
        {
            kX = 0, /**< X 轴 */
            kY = 1  /**< Y 轴 */
        };
        /** @brief 轴数量 */
        static constexpr int32 kAxisCount = 2;

        union
        {
            struct
            {
                float32 x; /**< X 分量 */
                float32 y; /**< Y 分量 */
            };
            struct
            {
                float32 width;  /**< 宽度（与 x 共享） */
                float32 height; /**< 高度（与 y 共享） */
            };
            float32 coord[2]; /**< 分量数组访问 */
        };

        /** @brief 按轴索引访问（可写）
         *  @param[in] p_axis 轴索引 (0=X, 1=Y)
         *  @return 对应分量的引用 */
        ARHUD_ALWAYS_INLINE float32 &operator[](int32 p_axis) { return coord[p_axis]; }
        /** @brief 按轴索引访问（只读）
         *  @param[in] p_axis 轴索引 (0=X, 1=Y)
         *  @return 对应分量的常量引用 */
        ARHUD_ALWAYS_INLINE const float32 &operator[](int32 p_axis) const { return coord[p_axis]; }

        /** @brief 获取最小分量的轴索引
         *  @return 如果 x < y 返回 kX，否则 kY */
        ARHUD_ALWAYS_INLINE Axis MinAxisIndex() const { return x < y ? Axis::kX : Axis::kY; }
        /** @brief 获取最大分量的轴索引
         *  @return 如果 x < y 返回 kY，否则 kX */
        ARHUD_ALWAYS_INLINE Axis MaxAxisIndex() const { return x < y ? Axis::kY : Axis::kX; }

        /** @brief 向量长度（模）
         *  @return √(x² + y²) */
        ARHUD_ALWAYS_INLINE float32 Length() const { return MathFuncs::Sqrt(x * x + y * y); }
        /** @brief 向量长度的平方
         *  @return x² + y² */
        ARHUD_ALWAYS_INLINE float32 LengthSquared() const { return x * x + y * y; }

        /** @brief 原地归一化（单位向量化）
         *  @note 零向量不做任何操作 */
        ARHUD_ALWAYS_INLINE void Normalize()
        {
            float32 len = Length();
            if (len > 0.0f)
            {
                x /= len;
                y /= len;
            }
        }

        /** @brief 返回归一化副本
         *  @return 单位向量，零向量返回零向量 */
        ARHUD_ALWAYS_INLINE Vec2 Normalized() const
        {
            Vec2 v = *this;
            v.Normalize();
            return v;
        }

        /** @brief 判断是否为归一化向量
         *  @return true 当 |length² - 1| < kEpsilon² */
        ARHUD_ALWAYS_INLINE bool IsNormalized() const
        {
            return MathFuncs::IsEqualApprox(LengthSquared(), 1.0f, MathFuncs::kEpsilon2);
        }

        /** @brief 点积
         *  @param[in] p_other 另一个向量
         *  @return x·other.x + y·other.y */
        ARHUD_ALWAYS_INLINE float32 Dot(const Vec2 &p_other) const
        {
            return x * p_other.x + y * p_other.y;
        }

        /** @brief 二维叉积（标量）
         *  @param[in] p_other 另一个向量
         *  @return x·other.y - y·other.x */
        ARHUD_ALWAYS_INLINE float32 Cross(const Vec2 &p_other) const
        {
            return x * p_other.y - y * p_other.x;
        }

        /** @brief 计算指向目标的方向向量
         *  @param[in] p_to 目标点
         *  @return 从自身指向 p_to 的单位向量 */
        ARHUD_ALWAYS_INLINE Vec2 DirectionTo(const Vec2 &p_to) const
        {
            Vec2 diff = p_to - *this;
            float32 len = diff.Length();
            return len > 0.0f ? diff / len : Vec2(0.0f, 0.0f);
        }

        /** @brief 到目标点的距离
         *  @param[in] p_to 目标点
         *  @return |p_to - this| */
        ARHUD_ALWAYS_INLINE float32 DistanceTo(const Vec2 &p_to) const
        {
            return (*this - p_to).Length();
        }

        /** @brief 到目标点距离的平方
         *  @param[in] p_to 目标点
         *  @return |p_to - this|² */
        ARHUD_ALWAYS_INLINE float32 DistanceSquaredTo(const Vec2 &p_to) const
        {
            return (*this - p_to).LengthSquared();
        }

        /** @brief 向量极角
         *  @return atan2(y, x)，范围 [-π, π] */
        ARHUD_ALWAYS_INLINE float32 Angle() const { return MathFuncs::Atan2(y, x); }

        /** @brief 到另一向量的有符号角度
         *  @param[in] p_to 目标向量
         *  @return this 到 p_to 的旋转角 */
        ARHUD_ALWAYS_INLINE float32 AngleTo(const Vec2 &p_to) const
        {
            return MathFuncs::Atan2(Cross(p_to), Dot(p_to));
        }

        /** @brief 到空间一点的角度
         *  @param[in] p_to 目标点
         *  @return 从自身指向 p_to 的极角 */
        ARHUD_ALWAYS_INLINE float32 AngleToPoint(const Vec2 &p_to) const
        {
            return MathFuncs::Atan2(y - p_to.y, x - p_to.x);
        }

        /** @brief 向量线性插值
         *  @param[in] p_to 目标向量
         *  @param[in] p_weight 插值权重 [0, 1]
         *  @return this + (p_to - this) * p_weight */
        ARHUD_ALWAYS_INLINE Vec2 Lerp(const Vec2 &p_to, float32 p_weight) const
        {
            return Vec2(MathFuncs::Lerp(x, p_to.x, p_weight),
                        MathFuncs::Lerp(y, p_to.y, p_weight));
        }

        /** @brief 以固定步长移向目标
         *  @param[in] p_to 目标位置
         *  @param[in] p_delta 最大移动距离
         *  @return 向目标移动后的新位置 */
        ARHUD_ALWAYS_INLINE Vec2 MoveToward(const Vec2 &p_to, float32 p_delta) const
        {
            Vec2 diff = p_to - *this;
            float32 len = diff.Length();
            if (len <= p_delta || len < MathFuncs::kEpsilon)
            {
                return p_to;
            }
            return *this + diff / len * p_delta;
        }

        /** @brief 绕原点旋转
         *  @param[in] p_angle 旋转角度（弧度）
         *  @return 旋转后的向量 */
        ARHUD_ALWAYS_INLINE Vec2 Rotated(float32 p_angle) const
        {
            float32 cs = MathFuncs::Cos(p_angle);
            float32 sn = MathFuncs::Sin(p_angle);
            return Vec2(x * cs - y * sn, x * sn + y * cs);
        }

        /** @brief 返回垂直向量（逆时针 90°）
         *  @return (y, -x) */
        ARHUD_ALWAYS_INLINE Vec2 Orthogonal() const { return Vec2(y, -x); }
        /** @brief 沿法线方向滑动
         *  @param[in] p_normal 法线（需归一化）
         *  @return 去除法线方向分量后的向量 */
        ARHUD_ALWAYS_INLINE Vec2 Slide(const Vec2 &p_normal) const { return *this - p_normal * Dot(p_normal); }
        /** @brief 沿法线反弹
         *  @param[in] p_normal 法线（需归一化）
         *  @return -Reflect(p_normal) */
        ARHUD_ALWAYS_INLINE Vec2 Bounce(const Vec2 &p_normal) const { return -Reflect(p_normal); }
        /** @brief 沿法线反射
         *  @param[in] p_normal 法线（需归一化）
         *  @return this - 2 * normal * Dot(normal) */
        ARHUD_ALWAYS_INLINE Vec2 Reflect(const Vec2 &p_normal) const { return *this - p_normal * 2.0f * Dot(p_normal); }

        /** @brief 向量投影
         *  @param[in] p_to 投影目标方向
         *  @return 在 p_to 方向上的投影向量 */
        ARHUD_ALWAYS_INLINE Vec2 Project(const Vec2 &p_to) const
        {
            return p_to * (Dot(p_to) / p_to.LengthSquared());
        }

        /** @brief 分量绝对值
         *  @return (|x|, |y|) */
        ARHUD_ALWAYS_INLINE Vec2 Abs() const { return Vec2(MathFuncs::Abs(x), MathFuncs::Abs(y)); }
        /** @brief 分量符号
         *  @return (sign(x), sign(y)) */
        ARHUD_ALWAYS_INLINE Vec2 Sign() const { return Vec2(MathFuncs::Sign(x), MathFuncs::Sign(y)); }
        /** @brief 分量向下取整
         *  @return (floor(x), floor(y)) */
        ARHUD_ALWAYS_INLINE Vec2 Floor() const { return Vec2(MathFuncs::Floor(x), MathFuncs::Floor(y)); }
        /** @brief 分量向上取整
         *  @return (ceil(x), ceil(y)) */
        ARHUD_ALWAYS_INLINE Vec2 Ceil() const { return Vec2(MathFuncs::Ceil(x), MathFuncs::Ceil(y)); }
        /** @brief 分量四舍五入
         *  @return (round(x), round(y)) */
        ARHUD_ALWAYS_INLINE Vec2 Round() const { return Vec2(MathFuncs::Round(x), MathFuncs::Round(y)); }

        /** @brief 按步长对齐（向下取整）
         *  @param[in] p_step 对齐步长
         *  @return 对齐后的向量 */
        ARHUD_ALWAYS_INLINE Vec2 Snapped(const Vec2 &p_step) const
        {
            return Vec2(MathFuncs::Floor(x / p_step.x) * p_step.x,
                        MathFuncs::Floor(y / p_step.y) * p_step.y);
        }

        /** @brief 分量钳位（向量边界）
         *  @param[in] p_min 下限向量
         *  @param[in] p_max 上限向量
         *  @return 钳位后的向量 */
        ARHUD_ALWAYS_INLINE Vec2 Clamp(const Vec2 &p_min, const Vec2 &p_max) const
        {
            return Vec2(MathFuncs::Clamp(x, p_min.x, p_max.x),
                        MathFuncs::Clamp(y, p_min.y, p_max.y));
        }

        /** @brief 分量统一钳位（标量边界）
         *  @param[in] p_min 下限
         *  @param[in] p_max 上限
         *  @return 钳位后的向量 */
        ARHUD_ALWAYS_INLINE Vec2 Clampf(float32 p_min, float32 p_max) const
        {
            return Vec2(MathFuncs::Clamp(x, p_min, p_max), MathFuncs::Clamp(y, p_min, p_max));
        }

        /** @brief 分量最小值
         *  @param[in] p_other 另一个向量
         *  @return (min(x, ox), min(y, oy)) */
        ARHUD_ALWAYS_INLINE Vec2 Min(const Vec2 &p_other) const
        {
            return Vec2(MathFuncs::Min(x, p_other.x), MathFuncs::Min(y, p_other.y));
        }

        /** @brief 分量最大值
         *  @param[in] p_other 另一个向量
         *  @return (max(x, ox), max(y, oy)) */
        ARHUD_ALWAYS_INLINE Vec2 Max(const Vec2 &p_other) const
        {
            return Vec2(MathFuncs::Max(x, p_other.x), MathFuncs::Max(y, p_other.y));
        }

        /** @brief 近似相等判断
         *  @param[in] p_v 比较向量
         *  @return true 当各分量均近似相等 */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(const Vec2 &p_v) const
        {
            return MathFuncs::IsEqualApprox(x, p_v.x) && MathFuncs::IsEqualApprox(y, p_v.y);
        }

        /** @brief 近似零向量判断
         *  @return true 当各分量均近似为零 */
        ARHUD_ALWAYS_INLINE bool IsZeroApprox() const
        {
            return MathFuncs::IsZeroApprox(x) && MathFuncs::IsZeroApprox(y);
        }

        /** @brief 有限性判断
         *  @return true 当所有分量均为有限值 */
        ARHUD_ALWAYS_INLINE bool IsFinite() const
        {
            return MathFuncs::IsFinite(x) && MathFuncs::IsFinite(y);
        }

        /** @brief 宽高比
         *  @return width / height */
        ARHUD_ALWAYS_INLINE float32 Aspect() const { return width / height; }

        /** @brief 限制向量最大长度
         *  @param[in] p_len 最大长度（默认 1.0）
         *  @return 截断后的向量 */
        ARHUD_ALWAYS_INLINE Vec2 LimitLength(float32 p_len = 1.0f) const
        {
            float32 len = Length();
            return (len > 0.0f && len > p_len) ? *this * (p_len / len) : *this;
        }

        /** @brief 从角度构造单位向量
         *  @param[in] p_angle 角度（弧度）
         *  @return (cos(angle), sin(angle)) */
        static ARHUD_ALWAYS_INLINE Vec2 FromAngle(float32 p_angle)
        {
            return Vec2(MathFuncs::Cos(p_angle), MathFuncs::Sin(p_angle));
        }

        /** @brief 逐分量加法 */
        constexpr Vec2 operator+(const Vec2 &p_v) const { return Vec2(x + p_v.x, y + p_v.y); }
        /** @brief 逐分量减法 */
        constexpr Vec2 operator-(const Vec2 &p_v) const { return Vec2(x - p_v.x, y - p_v.y); }
        /** @brief 逐分量乘法 */
        constexpr Vec2 operator*(const Vec2 &p_v) const { return Vec2(x * p_v.x, y * p_v.y); }
        /** @brief 逐分量除法 */
        constexpr Vec2 operator/(const Vec2 &p_v) const { return Vec2(x / p_v.x, y / p_v.y); }
        /** @brief 标量乘法 */
        constexpr Vec2 operator*(float32 p_s) const { return Vec2(x * p_s, y * p_s); }
        /** @brief 标量除法 */
        constexpr Vec2 operator/(float32 p_s) const { return Vec2(x / p_s, y / p_s); }
        /** @brief 取负 */
        constexpr Vec2 operator-() const { return Vec2(-x, -y); }

        /** @brief 逐分量加法赋值 */
        constexpr Vec2 &operator+=(const Vec2 &p_v)
        {
            x += p_v.x;
            y += p_v.y;
            return *this;
        }
        /** @brief 逐分量减法赋值 */
        constexpr Vec2 &operator-=(const Vec2 &p_v)
        {
            x -= p_v.x;
            y -= p_v.y;
            return *this;
        }
        /** @brief 标量乘法赋值 */
        constexpr Vec2 &operator*=(float32 p_s)
        {
            x *= p_s;
            y *= p_s;
            return *this;
        }
        /** @brief 标量除法赋值 */
        constexpr Vec2 &operator/=(float32 p_s)
        {
            x /= p_s;
            y /= p_s;
            return *this;
        }
        /** @brief 逐分量乘法赋值 */
        constexpr Vec2 &operator*=(const Vec2 &p_v)
        {
            x *= p_v.x;
            y *= p_v.y;
            return *this;
        }
        /** @brief 逐分量除法赋值 */
        constexpr Vec2 &operator/=(const Vec2 &p_v)
        {
            x /= p_v.x;
            y /= p_v.y;
            return *this;
        }

        /** @brief 相等比较 */
        constexpr bool operator==(const Vec2 &p_v) const { return x == p_v.x && y == p_v.y; }
        /** @brief 不等比较 */
        constexpr bool operator!=(const Vec2 &p_v) const { return x != p_v.x || y != p_v.y; }
        /** @brief 字典序小于比较 */
        constexpr bool operator<(const Vec2 &p_v) const { return x == p_v.x ? (y < p_v.y) : (x < p_v.x); }
        /** @brief 字典序大于比较 */
        constexpr bool operator>(const Vec2 &p_v) const { return x == p_v.x ? (y > p_v.y) : (x > p_v.x); }
        /** @brief 字典序小于等于 */
        constexpr bool operator<=(const Vec2 &p_v) const { return x == p_v.x ? (y <= p_v.y) : (x < p_v.x); }
        /** @brief 字典序大于等于 */
        constexpr bool operator>=(const Vec2 &p_v) const { return x == p_v.x ? (y >= p_v.y) : (x > p_v.x); }

        /** @brief 默认构造零向量 (0, 0) */
        constexpr Vec2() : x(0.0f), y(0.0f) {}
        /** @brief 从分量构造
         *  @param[in] p_x X 分量
         *  @param[in] p_y Y 分量 */
        constexpr Vec2(float32 p_x, float32 p_y) : x(p_x), y(p_y) {}

        /** @brief 转换为 Vec2i（四舍五入） */
        operator Vec2i() const;
    };

    inline constexpr Vec2 Vec2::kLeft = Vec2(-1.0f, 0.0f);
    inline constexpr Vec2 Vec2::kRight = Vec2(1.0f, 0.0f);
    inline constexpr Vec2 Vec2::kUp = Vec2(0.0f, -1.0f);
    inline constexpr Vec2 Vec2::kDown = Vec2(0.0f, 1.0f);
    inline constexpr Vec2 Vec2::kZero = Vec2(0.0f, 0.0f);
    inline constexpr Vec2 Vec2::kOne = Vec2(1.0f, 1.0f);

    constexpr Vec2 operator*(float32 p_s, const Vec2 &p_v) { return p_v * p_s; }

    // ═══════════════════════════════════════════════════════════════════════
    // Vec2i — 2D 整数向量
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 2D 整数向量
     *
     * 用于像素坐标、网格索引等整数场景。
     */
    struct Vec2i
    {
        /** @brief 零向量 (0, 0) */
        static const Vec2i kZero;
        /** @brief 单位向量 (1, 1) */
        static const Vec2i kOne;

        /** @brief 轴枚举 */
        enum class Axis : int32
        {
            kX = 0, /**< X 轴 */
            kY = 1  /**< Y 轴 */
        };
        /** @brief 轴数量 */
        static constexpr int32 kAxisCount = 2;

        union
        {
            struct
            {
                int32 x; /**< X 分量 */
                int32 y; /**< Y 分量 */
            };
            int32 coord[2]; /**< 分量数组访问 */
        };

        /** @brief 按轴索引访问（可写）
         *  @param[in] p_axis 轴索引 (0=X, 1=Y)
         *  @return 对应分量的引用 */
        ARHUD_ALWAYS_INLINE int32 &operator[](int32 p_axis) { return coord[p_axis]; }
        /** @brief 按轴索引访问（只读）
         *  @param[in] p_axis 轴索引 (0=X, 1=Y)
         *  @return 对应分量的常量引用 */
        ARHUD_ALWAYS_INLINE const int32 &operator[](int32 p_axis) const { return coord[p_axis]; }

        /** @brief 向量长度（浮点结果）
         *  @return √(x² + y²) */
        ARHUD_ALWAYS_INLINE float32 Length() const
        {
            return MathFuncs::Sqrt(static_cast<float32>(x * x + y * y));
        }

        /** @brief 向量长度平方
         *  @return x² + y² */
        ARHUD_ALWAYS_INLINE int32 LengthSquared() const { return x * x + y * y; }
        /** @brief 分量绝对值
         *  @return (|x|, |y|) */
        ARHUD_ALWAYS_INLINE Vec2i Abs() const { return Vec2i(MathFuncs::Abs(x), MathFuncs::Abs(y)); }
        /** @brief 分量符号
         *  @return (sign(x), sign(y)) */
        ARHUD_ALWAYS_INLINE Vec2i Sign() const { return Vec2i(MathFuncs::Sign(x), MathFuncs::Sign(y)); }
        /** @brief 逐分量最小值
         *  @param[in] p_other 另一个向量
         *  @return (min(x, ox), min(y, oy)) */
        ARHUD_ALWAYS_INLINE Vec2i Min(const Vec2i &p_other) const
        {
            return Vec2i(MathFuncs::Min(x, p_other.x), MathFuncs::Min(y, p_other.y));
        }
        /** @brief 逐分量最大值
         *  @param[in] p_other 另一个向量
         *  @return (max(x, ox), max(y, oy)) */
        ARHUD_ALWAYS_INLINE Vec2i Max(const Vec2i &p_other) const
        {
            return Vec2i(MathFuncs::Max(x, p_other.x), MathFuncs::Max(y, p_other.y));
        }

        /** @brief 逐分量加法 */
        constexpr Vec2i operator+(const Vec2i &p_v) const { return Vec2i(x + p_v.x, y + p_v.y); }
        /** @brief 逐分量减法 */
        constexpr Vec2i operator-(const Vec2i &p_v) const { return Vec2i(x - p_v.x, y - p_v.y); }
        /** @brief 逐分量乘法 */
        constexpr Vec2i operator*(const Vec2i &p_v) const { return Vec2i(x * p_v.x, y * p_v.y); }
        /** @brief 逐分量除法 */
        constexpr Vec2i operator/(const Vec2i &p_v) const { return Vec2i(x / p_v.x, y / p_v.y); }
        /** @brief 标量乘法 */
        constexpr Vec2i operator*(int32 p_s) const { return Vec2i(x * p_s, y * p_s); }
        /** @brief 标量除法 */
        constexpr Vec2i operator/(int32 p_s) const { return Vec2i(x / p_s, y / p_s); }
        /** @brief 取负 */
        constexpr Vec2i operator-() const { return Vec2i(-x, -y); }

        /** @brief 逐分量加法赋值 */
        constexpr Vec2i &operator+=(const Vec2i &p_v)
        {
            x += p_v.x;
            y += p_v.y;
            return *this;
        }
        /** @brief 逐分量减法赋值 */
        constexpr Vec2i &operator-=(const Vec2i &p_v)
        {
            x -= p_v.x;
            y -= p_v.y;
            return *this;
        }
        /** @brief 标量乘法赋值 */
        constexpr Vec2i &operator*=(int32 p_s)
        {
            x *= p_s;
            y *= p_s;
            return *this;
        }
        /** @brief 标量除法赋值 */
        constexpr Vec2i &operator/=(int32 p_s)
        {
            x /= p_s;
            y /= p_s;
            return *this;
        }

        /** @brief 相等比较 */
        constexpr bool operator==(const Vec2i &p_v) const { return x == p_v.x && y == p_v.y; }
        /** @brief 不等比较 */
        constexpr bool operator!=(const Vec2i &p_v) const { return x != p_v.x || y != p_v.y; }

        /** @brief 默认构造零向量 (0, 0) */
        constexpr Vec2i() : x(0), y(0) {}
        /** @brief 从分量构造
         *  @param[in] p_x X 分量
         *  @param[in] p_y Y 分量 */
        constexpr Vec2i(int32 p_x, int32 p_y) : x(p_x), y(p_y) {}

        /** @brief 转换为 Vec2（浮点转换） */
        operator Vec2() const { return Vec2(static_cast<float32>(x), static_cast<float32>(y)); }
    };

    inline constexpr Vec2i Vec2i::kZero = Vec2i(0, 0);
    inline constexpr Vec2i Vec2i::kOne = Vec2i(1, 1);

    inline Vec2::operator Vec2i() const
    {
        return Vec2i(static_cast<int32>(MathFuncs::Round(x)),
                     static_cast<int32>(MathFuncs::Round(y)));
    }

    constexpr Vec2i operator*(int32 p_s, const Vec2i &p_v) { return p_v * p_s; }

    // ═══════════════════════════════════════════════════════════════════════
    // Vec3 — 3D 浮点向量
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 3D 浮点向量
     *
     * 用于 3D 坐标、方向向量等场景。AR HUD 场景中主要用于透视投影计算。
     */
    struct Vec3
    {
        /** @brief 单位左向量 (-1, 0, 0) */
        static const Vec3 kLeft;
        /** @brief 单位右向量 (1, 0, 0) */
        static const Vec3 kRight;
        /** @brief 单位上向量 (0, 1, 0) */
        static const Vec3 kUp;
        /** @brief 单位下向量 (0, -1, 0) */
        static const Vec3 kDown;
        /** @brief 单位前向量 (0, 0, -1) */
        static const Vec3 kForward;
        /** @brief 单位后向量 (0, 0, 1) */
        static const Vec3 kBack;
        /** @brief 零向量 (0, 0, 0) */
        static const Vec3 kZero;
        /** @brief 单位向量 (1, 1, 1) */
        static const Vec3 kOne;

        /** @brief 轴枚举 */
        enum class Axis : int32
        {
            kX = 0, /**< X 轴 */
            kY = 1, /**< Y 轴 */
            kZ = 2  /**< Z 轴 */
        };
        /** @brief 轴数量 */
        static constexpr int32 kAxisCount = 3;

        union
        {
            struct
            {
                float32 x; /**< X 分量 */
                float32 y; /**< Y 分量 */
                float32 z; /**< Z 分量 */
            };
            float32 coord[3]; /**< 分量数组访问 */
        };

        /** @brief 按轴索引访问（可写）
         *  @param[in] p_axis 轴索引 (0=X, 1=Y, 2=Z)
         *  @return 对应分量的引用 */
        ARHUD_ALWAYS_INLINE float32 &operator[](int32 p_axis) { return coord[p_axis]; }
        /** @brief 按轴索引访问（只读）
         *  @param[in] p_axis 轴索引 (0=X, 1=Y, 2=Z)
         *  @return 对应分量的常量引用 */
        ARHUD_ALWAYS_INLINE const float32 &operator[](int32 p_axis) const { return coord[p_axis]; }

        /** @brief 获取最小分量的轴索引
         *  @return 值最小的分量对应的 Axis */
        ARHUD_ALWAYS_INLINE Axis MinAxisIndex() const
        {
            return x < y ? (x < z ? Axis::kX : Axis::kZ) : (y < z ? Axis::kY : Axis::kZ);
        }

        /** @brief 获取最大分量的轴索引
         *  @return 值最大的分量对应的 Axis */
        ARHUD_ALWAYS_INLINE Axis MaxAxisIndex() const
        {
            return x < y ? (y < z ? Axis::kZ : Axis::kY) : (x < z ? Axis::kZ : Axis::kX);
        }

        /** @brief 向量长度
         *  @return √(x² + y² + z²) */
        ARHUD_ALWAYS_INLINE float32 Length() const { return MathFuncs::Sqrt(x * x + y * y + z * z); }
        /** @brief 向量长度平方
         *  @return x² + y² + z² */
        ARHUD_ALWAYS_INLINE float32 LengthSquared() const { return x * x + y * y + z * z; }

        /** @brief 原地归一化
         *  @note 零向量不做任何操作 */
        ARHUD_ALWAYS_INLINE void Normalize()
        {
            float32 len = Length();
            if (len > 0.0f)
            {
                x /= len;
                y /= len;
                z /= len;
            }
        }

        /** @brief 返回归一化副本
         *  @return 单位向量，零向量返回零向量 */
        ARHUD_ALWAYS_INLINE Vec3 Normalized() const
        {
            Vec3 v = *this;
            v.Normalize();
            return v;
        }

        /** @brief 判断是否为归一化向量
         *  @return true 当 |length² - 1| < kEpsilon² */
        ARHUD_ALWAYS_INLINE bool IsNormalized() const
        {
            return MathFuncs::IsEqualApprox(LengthSquared(), 1.0f, MathFuncs::kEpsilon2);
        }

        /** @brief 点积
         *  @param[in] p_other 另一个向量
         *  @return x·ox + y·oy + z·oz */
        ARHUD_ALWAYS_INLINE float32 Dot(const Vec3 &p_other) const
        {
            return x * p_other.x + y * p_other.y + z * p_other.z;
        }

        /** @brief 三维叉积
         *  @param[in] p_other 另一个向量
         *  @return this × p_other */
        ARHUD_ALWAYS_INLINE Vec3 Cross(const Vec3 &p_other) const
        {
            return Vec3(y * p_other.z - z * p_other.y,
                        z * p_other.x - x * p_other.z,
                        x * p_other.y - y * p_other.x);
        }

        /** @brief 指向目标的方向向量
         *  @param[in] p_to 目标点
         *  @return 从自身指向 p_to 的单位向量 */
        ARHUD_ALWAYS_INLINE Vec3 DirectionTo(const Vec3 &p_to) const
        {
            Vec3 diff = p_to - *this;
            float32 len = diff.Length();
            return len > 0.0f ? diff / len : Vec3(0.0f, 0.0f, 0.0f);
        }

        /** @brief 到目标点的距离
         *  @param[in] p_to 目标点
         *  @return |p_to - this| */
        ARHUD_ALWAYS_INLINE float32 DistanceTo(const Vec3 &p_to) const { return (*this - p_to).Length(); }
        /** @brief 到目标点距离的平方
         *  @param[in] p_to 目标点
         *  @return |p_to - this|² */
        ARHUD_ALWAYS_INLINE float32 DistanceSquaredTo(const Vec3 &p_to) const { return (*this - p_to).LengthSquared(); }

        /** @brief 向量线性插值
         *  @param[in] p_to 目标向量
         *  @param[in] p_weight 插值权重 [0, 1]
         *  @return this + (p_to - this) * p_weight */
        ARHUD_ALWAYS_INLINE Vec3 Lerp(const Vec3 &p_to, float32 p_weight) const
        {
            return Vec3(MathFuncs::Lerp(x, p_to.x, p_weight),
                        MathFuncs::Lerp(y, p_to.y, p_weight),
                        MathFuncs::Lerp(z, p_to.z, p_weight));
        }

        /** @brief 以固定步长移向目标
         *  @param[in] p_to 目标位置
         *  @param[in] p_delta 最大移动距离
         *  @return 向目标移动后的新位置 */
        ARHUD_ALWAYS_INLINE Vec3 MoveToward(const Vec3 &p_to, float32 p_delta) const
        {
            Vec3 diff = p_to - *this;
            float32 len = diff.Length();
            if (len <= p_delta || len < MathFuncs::kEpsilon)
            {
                return p_to;
            }
            return *this + diff / len * p_delta;
        }

        /** @brief 分量绝对值
         *  @return (|x|, |y|, |z|) */
        ARHUD_ALWAYS_INLINE Vec3 Abs() const { return Vec3(MathFuncs::Abs(x), MathFuncs::Abs(y), MathFuncs::Abs(z)); }
        /** @brief 分量符号
         *  @return (sign(x), sign(y), sign(z)) */
        ARHUD_ALWAYS_INLINE Vec3 Sign() const { return Vec3(MathFuncs::Sign(x), MathFuncs::Sign(y), MathFuncs::Sign(z)); }
        /** @brief 分量向下取整
         *  @return (floor(x), floor(y), floor(z)) */
        ARHUD_ALWAYS_INLINE Vec3 Floor() const { return Vec3(MathFuncs::Floor(x), MathFuncs::Floor(y), MathFuncs::Floor(z)); }
        /** @brief 分量向上取整
         *  @return (ceil(x), ceil(y), ceil(z)) */
        ARHUD_ALWAYS_INLINE Vec3 Ceil() const { return Vec3(MathFuncs::Ceil(x), MathFuncs::Ceil(y), MathFuncs::Ceil(z)); }
        /** @brief 分量四舍五入
         *  @return (round(x), round(y), round(z)) */
        ARHUD_ALWAYS_INLINE Vec3 Round() const { return Vec3(MathFuncs::Round(x), MathFuncs::Round(y), MathFuncs::Round(z)); }

        /** @brief 分量钳位（向量边界）
         *  @param[in] p_min 下限向量
         *  @param[in] p_max 上限向量
         *  @return 钳位后的向量 */
        ARHUD_ALWAYS_INLINE Vec3 Clamp(const Vec3 &p_min, const Vec3 &p_max) const
        {
            return Vec3(MathFuncs::Clamp(x, p_min.x, p_max.x),
                        MathFuncs::Clamp(y, p_min.y, p_max.y),
                        MathFuncs::Clamp(z, p_min.z, p_max.z));
        }

        /** @brief 分量统一钳位（标量边界）
         *  @param[in] p_min 下限
         *  @param[in] p_max 上限
         *  @return 钳位后的向量 */
        ARHUD_ALWAYS_INLINE Vec3 Clampf(float32 p_min, float32 p_max) const
        {
            return Vec3(MathFuncs::Clamp(x, p_min, p_max),
                        MathFuncs::Clamp(y, p_min, p_max),
                        MathFuncs::Clamp(z, p_min, p_max));
        }

        /** @brief 逐分量最小值
         *  @param[in] p_other 另一个向量
         *  @return (min(x, ox), min(y, oy), min(z, oz)) */
        ARHUD_ALWAYS_INLINE Vec3 Min(const Vec3 &p_other) const
        {
            return Vec3(MathFuncs::Min(x, p_other.x), MathFuncs::Min(y, p_other.y),
                        MathFuncs::Min(z, p_other.z));
        }

        /** @brief 逐分量最大值
         *  @param[in] p_other 另一个向量
         *  @return (max(x, ox), max(y, oy), max(z, oz)) */
        ARHUD_ALWAYS_INLINE Vec3 Max(const Vec3 &p_other) const
        {
            return Vec3(MathFuncs::Max(x, p_other.x), MathFuncs::Max(y, p_other.y),
                        MathFuncs::Max(z, p_other.z));
        }

        /** @brief 沿法线方向滑动
         *  @param[in] p_normal 法线（需归一化）
         *  @return 去除法线方向分量后的向量 */
        ARHUD_ALWAYS_INLINE Vec3 Slide(const Vec3 &p_normal) const { return *this - p_normal * Dot(p_normal); }
        /** @brief 沿法线反射
         *  @param[in] p_normal 法线（需归一化）
         *  @return this - 2 * normal * Dot(normal) */
        ARHUD_ALWAYS_INLINE Vec3 Reflect(const Vec3 &p_normal) const { return *this - p_normal * 2.0f * Dot(p_normal); }
        /** @brief 沿法线反弹
         *  @param[in] p_normal 法线（需归一化）
         *  @return -Reflect(p_normal) */
        ARHUD_ALWAYS_INLINE Vec3 Bounce(const Vec3 &p_normal) const { return -Reflect(p_normal); }

        /** @brief 向量投影
         *  @param[in] p_to 投影目标方向
         *  @return 在 p_to 方向上的投影向量 */
        ARHUD_ALWAYS_INLINE Vec3 Project(const Vec3 &p_to) const
        {
            return p_to * (Dot(p_to) / p_to.LengthSquared());
        }

        /** @brief 近似相等判断
         *  @param[in] p_v 比较向量
         *  @return true 当各分量均近似相等 */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(const Vec3 &p_v) const
        {
            return MathFuncs::IsEqualApprox(x, p_v.x) &&
                   MathFuncs::IsEqualApprox(y, p_v.y) &&
                   MathFuncs::IsEqualApprox(z, p_v.z);
        }

        /** @brief 近似零向量判断
         *  @return true 当各分量均近似为零 */
        ARHUD_ALWAYS_INLINE bool IsZeroApprox() const
        {
            return MathFuncs::IsZeroApprox(x) && MathFuncs::IsZeroApprox(y) && MathFuncs::IsZeroApprox(z);
        }

        /** @brief 有限性判断
         *  @return true 当所有分量均为有限值 */
        ARHUD_ALWAYS_INLINE bool IsFinite() const
        {
            return MathFuncs::IsFinite(x) && MathFuncs::IsFinite(y) && MathFuncs::IsFinite(z);
        }

        /** @brief 逐分量加法 */
        constexpr Vec3 operator+(const Vec3 &p_v) const { return Vec3(x + p_v.x, y + p_v.y, z + p_v.z); }
        /** @brief 逐分量减法 */
        constexpr Vec3 operator-(const Vec3 &p_v) const { return Vec3(x - p_v.x, y - p_v.y, z - p_v.z); }
        /** @brief 逐分量乘法 */
        constexpr Vec3 operator*(const Vec3 &p_v) const { return Vec3(x * p_v.x, y * p_v.y, z * p_v.z); }
        /** @brief 逐分量除法 */
        constexpr Vec3 operator/(const Vec3 &p_v) const { return Vec3(x / p_v.x, y / p_v.y, z / p_v.z); }
        /** @brief 标量乘法 */
        constexpr Vec3 operator*(float32 p_s) const { return Vec3(x * p_s, y * p_s, z * p_s); }
        /** @brief 标量除法 */
        constexpr Vec3 operator/(float32 p_s) const { return Vec3(x / p_s, y / p_s, z / p_s); }
        /** @brief 取负 */
        constexpr Vec3 operator-() const { return Vec3(-x, -y, -z); }

        /** @brief 逐分量加法赋值 */
        constexpr Vec3 &operator+=(const Vec3 &p_v)
        {
            x += p_v.x;
            y += p_v.y;
            z += p_v.z;
            return *this;
        }
        /** @brief 逐分量减法赋值 */
        constexpr Vec3 &operator-=(const Vec3 &p_v)
        {
            x -= p_v.x;
            y -= p_v.y;
            z -= p_v.z;
            return *this;
        }
        /** @brief 标量乘法赋值 */
        constexpr Vec3 &operator*=(float32 p_s)
        {
            x *= p_s;
            y *= p_s;
            z *= p_s;
            return *this;
        }
        /** @brief 标量除法赋值 */
        constexpr Vec3 &operator/=(float32 p_s)
        {
            x /= p_s;
            y /= p_s;
            z /= p_s;
            return *this;
        }
        /** @brief 逐分量乘法赋值 */
        constexpr Vec3 &operator*=(const Vec3 &p_v)
        {
            x *= p_v.x;
            y *= p_v.y;
            z *= p_v.z;
            return *this;
        }
        /** @brief 逐分量除法赋值 */
        constexpr Vec3 &operator/=(const Vec3 &p_v)
        {
            x /= p_v.x;
            y /= p_v.y;
            z /= p_v.z;
            return *this;
        }

        /** @brief 相等比较 */
        constexpr bool operator==(const Vec3 &p_v) const { return x == p_v.x && y == p_v.y && z == p_v.z; }
        /** @brief 不等比较 */
        constexpr bool operator!=(const Vec3 &p_v) const { return x != p_v.x || y != p_v.y || z != p_v.z; }

        /** @brief 默认构造零向量 (0, 0, 0) */
        constexpr Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
        /** @brief 从分量构造
         *  @param[in] p_x X 分量
         *  @param[in] p_y Y 分量
         *  @param[in] p_z Z 分量 */
        constexpr Vec3(float32 p_x, float32 p_y, float32 p_z) : x(p_x), y(p_y), z(p_z) {}
    };

    inline constexpr Vec3 Vec3::kLeft = Vec3(-1.0f, 0.0f, 0.0f);
    inline constexpr Vec3 Vec3::kRight = Vec3(1.0f, 0.0f, 0.0f);
    inline constexpr Vec3 Vec3::kUp = Vec3(0.0f, 1.0f, 0.0f);
    inline constexpr Vec3 Vec3::kDown = Vec3(0.0f, -1.0f, 0.0f);
    inline constexpr Vec3 Vec3::kForward = Vec3(0.0f, 0.0f, -1.0f);
    inline constexpr Vec3 Vec3::kBack = Vec3(0.0f, 0.0f, 1.0f);
    inline constexpr Vec3 Vec3::kZero = Vec3(0.0f, 0.0f, 0.0f);
    inline constexpr Vec3 Vec3::kOne = Vec3(1.0f, 1.0f, 1.0f);

    constexpr Vec3 operator*(float32 p_s, const Vec3 &p_v) { return p_v * p_s; }

    // ═══════════════════════════════════════════════════════════════════════
    // Vec4 — 4D 浮点向量
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 4D 浮点向量
     *
     * 用于齐次坐标、四元数存储、Shader Uniform 传递等场景。
     */
    struct Vec4
    {
        /** @brief 零向量 (0, 0, 0, 0) */
        static const Vec4 kZero;
        /** @brief 单位向量 (1, 1, 1, 1) */
        static const Vec4 kOne;

        union
        {
            struct
            {
                float32 x; /**< X 分量 */
                float32 y; /**< Y 分量 */
                float32 z; /**< Z 分量 */
                float32 w; /**< W 分量 */
            };
            float32 coord[4]; /**< 分量数组访问 */
        };

        /** @brief 按索引访问（可写）
         *  @param[in] p_axis 索引 (0=X, 1=Y, 2=Z, 3=W)
         *  @return 对应分量的引用 */
        ARHUD_ALWAYS_INLINE float32 &operator[](int32 p_axis) { return coord[p_axis]; }
        /** @brief 按索引访问（只读）
         *  @param[in] p_axis 索引 (0=X, 1=Y, 2=Z, 3=W)
         *  @return 对应分量的常量引用 */
        ARHUD_ALWAYS_INLINE const float32 &operator[](int32 p_axis) const { return coord[p_axis]; }

        /** @brief 向量长度
         *  @return √(x² + y² + z² + w²) */
        ARHUD_ALWAYS_INLINE float32 Length() const { return MathFuncs::Sqrt(x * x + y * y + z * z + w * w); }
        /** @brief 向量长度平方
         *  @return x² + y² + z² + w² */
        ARHUD_ALWAYS_INLINE float32 LengthSquared() const { return x * x + y * y + z * z + w * w; }

        /** @brief 原地归一化
         *  @note 零向量不做任何操作 */
        ARHUD_ALWAYS_INLINE void Normalize()
        {
            float32 len = Length();
            if (len > 0.0f)
            {
                x /= len;
                y /= len;
                z /= len;
                w /= len;
            }
        }

        /** @brief 返回归一化副本
         *  @return 单位向量，零向量返回零向量 */
        ARHUD_ALWAYS_INLINE Vec4 Normalized() const
        {
            Vec4 v = *this;
            v.Normalize();
            return v;
        }

        /** @brief 点积
         *  @param[in] p_other 另一个向量
         *  @return x·ox + y·oy + z·oz + w·ow */
        ARHUD_ALWAYS_INLINE float32 Dot(const Vec4 &p_other) const
        {
            return x * p_other.x + y * p_other.y + z * p_other.z + w * p_other.w;
        }

        /** @brief 分量绝对值
         *  @return (|x|, |y|, |z|, |w|) */
        ARHUD_ALWAYS_INLINE Vec4 Abs() const
        {
            return Vec4(MathFuncs::Abs(x), MathFuncs::Abs(y), MathFuncs::Abs(z), MathFuncs::Abs(w));
        }

        /** @brief 向量线性插值
         *  @param[in] p_to 目标向量
         *  @param[in] p_weight 插值权重 [0, 1]
         *  @return 插值后的向量 */
        ARHUD_ALWAYS_INLINE Vec4 Lerp(const Vec4 &p_to, float32 p_weight) const
        {
            return Vec4(MathFuncs::Lerp(x, p_to.x, p_weight), MathFuncs::Lerp(y, p_to.y, p_weight),
                        MathFuncs::Lerp(z, p_to.z, p_weight), MathFuncs::Lerp(w, p_to.w, p_weight));
        }

        /** @brief 近似相等判断
         *  @param[in] p_v 比较向量
         *  @return true 当各分量均近似相等 */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(const Vec4 &p_v) const
        {
            return MathFuncs::IsEqualApprox(x, p_v.x) && MathFuncs::IsEqualApprox(y, p_v.y) &&
                   MathFuncs::IsEqualApprox(z, p_v.z) && MathFuncs::IsEqualApprox(w, p_v.w);
        }

        /** @brief 有限性判断
         *  @return true 当所有分量均为有限值 */
        ARHUD_ALWAYS_INLINE bool IsFinite() const
        {
            return MathFuncs::IsFinite(x) && MathFuncs::IsFinite(y) &&
                   MathFuncs::IsFinite(z) && MathFuncs::IsFinite(w);
        }

        /** @brief 逐分量加法 */
        constexpr Vec4 operator+(const Vec4 &p_v) const { return Vec4(x + p_v.x, y + p_v.y, z + p_v.z, w + p_v.w); }
        /** @brief 逐分量减法 */
        constexpr Vec4 operator-(const Vec4 &p_v) const { return Vec4(x - p_v.x, y - p_v.y, z - p_v.z, w - p_v.w); }
        /** @brief 逐分量乘法 */
        constexpr Vec4 operator*(const Vec4 &p_v) const { return Vec4(x * p_v.x, y * p_v.y, z * p_v.z, w * p_v.w); }
        /** @brief 逐分量除法 */
        constexpr Vec4 operator/(const Vec4 &p_v) const { return Vec4(x / p_v.x, y / p_v.y, z / p_v.z, w / p_v.w); }
        /** @brief 标量乘法 */
        constexpr Vec4 operator*(float32 p_s) const { return Vec4(x * p_s, y * p_s, z * p_s, w * p_s); }
        /** @brief 标量除法 */
        constexpr Vec4 operator/(float32 p_s) const { return Vec4(x / p_s, y / p_s, z / p_s, w / p_s); }
        /** @brief 取负 */
        constexpr Vec4 operator-() const { return Vec4(-x, -y, -z, -w); }

        /** @brief 逐分量加法赋值 */
        constexpr Vec4 &operator+=(const Vec4 &p_v)
        {
            x += p_v.x;
            y += p_v.y;
            z += p_v.z;
            w += p_v.w;
            return *this;
        }
        /** @brief 逐分量减法赋值 */
        constexpr Vec4 &operator-=(const Vec4 &p_v)
        {
            x -= p_v.x;
            y -= p_v.y;
            z -= p_v.z;
            w -= p_v.w;
            return *this;
        }
        /** @brief 标量乘法赋值 */
        constexpr Vec4 &operator*=(float32 p_s)
        {
            x *= p_s;
            y *= p_s;
            z *= p_s;
            w *= p_s;
            return *this;
        }
        /** @brief 标量除法赋值 */
        constexpr Vec4 &operator/=(float32 p_s)
        {
            x /= p_s;
            y /= p_s;
            z /= p_s;
            w /= p_s;
            return *this;
        }

        /** @brief 相等比较 */
        constexpr bool operator==(const Vec4 &p_v) const { return x == p_v.x && y == p_v.y && z == p_v.z && w == p_v.w; }
        /** @brief 不等比较 */
        constexpr bool operator!=(const Vec4 &p_v) const { return x != p_v.x || y != p_v.y || z != p_v.z || w != p_v.w; }

        /** @brief 默认构造零向量 (0, 0, 0, 0) */
        constexpr Vec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
        /** @brief 从分量构造
         *  @param[in] p_x X 分量
         *  @param[in] p_y Y 分量
         *  @param[in] p_z Z 分量
         *  @param[in] p_w W 分量 */
        constexpr Vec4(float32 p_x, float32 p_y, float32 p_z, float32 p_w) : x(p_x), y(p_y), z(p_z), w(p_w) {}
    };

    inline constexpr Vec4 Vec4::kZero = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    inline constexpr Vec4 Vec4::kOne = Vec4(1.0f, 1.0f, 1.0f, 1.0f);

    constexpr Vec4 operator*(float32 p_s, const Vec4 &p_v) { return p_v * p_s; }

    // ═══════════════════════════════════════════════════════════════════════
    // Size2i — 2D 整数尺寸
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 2D 整数尺寸
     *
     * 用于纹理尺寸、窗口大小、帧缓冲分辨率等整数尺寸场景。
     * 与 Vec2i 不同，Size2i 使用 width/height 语义，强调"尺寸"而非"向量"。
     */
    struct Size2i
    {
        /** @brief 宽度分量 */
        int32 width;
        /** @brief 高度分量 */
        int32 height;

        /**
         * @brief 计算面积
         * @return width * height
         */
        ARHUD_ALWAYS_INLINE int32 Area() const { return width * height; }

        /**
         * @brief 判断是否有有效面积
         * @return true 当 width > 0 且 height > 0
         */
        ARHUD_ALWAYS_INLINE bool HasArea() const { return width > 0 && height > 0; }

        /**
         * @brief 获取绝对尺寸
         * @return (|width|, |height|)
         */
        ARHUD_ALWAYS_INLINE Size2i Abs() const { return Size2i(MathFuncs::Abs(width), MathFuncs::Abs(height)); }

        /**
         * @brief 逐分量加法
         * @param[in] p_v 加向量
         * @return 新的尺寸
         */
        constexpr Size2i operator+(const Size2i &p_v) const { return Size2i(width + p_v.width, height + p_v.height); }

        /**
         * @brief 逐分量减法
         * @param[in] p_v 减向量
         * @return 新的尺寸
         */
        constexpr Size2i operator-(const Size2i &p_v) const { return Size2i(width - p_v.width, height - p_v.height); }

        /**
         * @brief 标量乘法
         * @param[in] p_s 标量值
         * @return 缩放后的尺寸
         */
        constexpr Size2i operator*(int32 p_s) const { return Size2i(width * p_s, height * p_s); }

        /**
         * @brief 标量除法
         * @param[in] p_s 标量值
         * @return 缩放后的尺寸
         */
        constexpr Size2i operator/(int32 p_s) const { return Size2i(width / p_s, height / p_s); }

        /**
         * @brief 取负
         * @return 负尺寸
         */
        constexpr Size2i operator-() const { return Size2i(-width, -height); }

        /**
         * @brief 逐分量加法赋值
         * @param[in] p_v 加向量
         * @return 自身引用
         */
        constexpr Size2i &operator+=(const Size2i &p_v)
        {
            width += p_v.width;
            height += p_v.height;
            return *this;
        }

        /**
         * @brief 逐分量减法赋值
         * @param[in] p_v 减向量
         * @return 自身引用
         */
        constexpr Size2i &operator-=(const Size2i &p_v)
        {
            width -= p_v.width;
            height -= p_v.height;
            return *this;
        }

        /**
         * @brief 相等比较
         * @param[in] p_v 比较对象
         * @return true 当 width 和 height 均相等
         */
        constexpr bool operator==(const Size2i &p_v) const { return width == p_v.width && height == p_v.height; }

        /**
         * @brief 不等比较
         * @param[in] p_v 比较对象
         * @return true 当 width 或 height 不等
         */
        constexpr bool operator!=(const Size2i &p_v) const { return width != p_v.width || height != p_v.height; }

        /**
         * @brief 默认构造
         * @details 初始化为 (0, 0)
         */
        constexpr Size2i() : width(0), height(0) {}

        /**
         * @brief 从宽高构造
         * @param[in] p_w 宽度
         * @param[in] p_h 高度
         */
        constexpr Size2i(int32 p_w, int32 p_h) : width(p_w), height(p_h) {}
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Rect2 — 2D 浮点矩形
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 2D 浮点矩形
     *
     * 由 position（左上角）和 size 定义。size 可为负数，Abs() 可规范化。
     * 用于 HUD 布局区域、裁剪矩形、纹理采样区域等。
     */
    struct Rect2
    {
        /** @brief 左上角位置 */
        Vec2 position;
        /** @brief 尺寸（可为负） */
        Vec2 size;

        /**
         * @brief 获取矩形中心点
         * @return position + size * 0.5
         */
        ARHUD_ALWAYS_INLINE Vec2 GetCenter() const { return position + size * 0.5f; }

        /**
         * @brief 设置矩形中心点
         * @param[in] p_center 新的中心点
         * @details 保持 size 不变，仅调整 position
         */
        ARHUD_ALWAYS_INLINE void SetCenter(const Vec2 &p_center) { position = p_center - size * 0.5f; }

        /**
         * @brief 获取矩形面积
         * @return size.x * size.y
         */
        ARHUD_ALWAYS_INLINE float32 GetArea() const { return size.x * size.y; }

        /**
         * @brief 判断是否有有效面积
         * @return true 当 size.x > 0 且 size.y > 0
         */
        ARHUD_ALWAYS_INLINE bool HasArea() const { return size.x > 0.0f && size.y > 0.0f; }

        /**
         * @brief 获取右下角点
         * @return position + size
         */
        ARHUD_ALWAYS_INLINE Vec2 GetEnd() const { return position + size; }

        /**
         * @brief 设置右下角点
         * @param[in] p_end 新的右下角点
         * @details 调整 size 使右下角到达 p_end
         */
        ARHUD_ALWAYS_INLINE void SetEnd(const Vec2 &p_end) { size = p_end - position; }

        /**
         * @brief 获取规范化矩形（size 均为正）
         * @return position 和 size 均为正的矩形
         */
        ARHUD_ALWAYS_INLINE Rect2 Abs() const
        {
            Vec2 pos = position;
            Vec2 sz = size;
            if (sz.x < 0.0f)
            {
                pos.x += sz.x;
                sz.x = -sz.x;
            }
            if (sz.y < 0.0f)
            {
                pos.y += sz.y;
                sz.y = -sz.y;
            }
            return Rect2(pos, sz);
        }

        /**
         * @brief 判断是否与另一矩形相交
         * @param[in] p_rect 另一矩形
         * @return true 当存在重叠区域
         */
        ARHUD_ALWAYS_INLINE bool Intersects(const Rect2 &p_rect) const
        {
            if (position.x >= (p_rect.position.x + p_rect.size.x))
            {
                return false;
            }
            if ((position.x + size.x) <= p_rect.position.x)
            {
                return false;
            }
            if (position.y >= (p_rect.position.y + p_rect.size.y))
            {
                return false;
            }
            if ((position.y + size.y) <= p_rect.position.y)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief 获取与另一矩形的交集
         * @param[in] p_rect 另一矩形
         * @return 两个矩形的重叠区域，若无重叠则 size 为负
         */
        ARHUD_ALWAYS_INLINE Rect2 Intersection(const Rect2 &p_rect) const
        {
            Rect2 r;
            r.position.x = MathFuncs::Max(position.x, p_rect.position.x);
            r.position.y = MathFuncs::Max(position.y, p_rect.position.y);
            Vec2 end;
            end.x = MathFuncs::Min(position.x + size.x, p_rect.position.x + p_rect.size.x);
            end.y = MathFuncs::Min(position.y + size.y, p_rect.position.y + p_rect.size.y);
            r.size = end - r.position;
            return r;
        }

        /**
         * @brief 判断是否完全包含另一矩形
         * @param[in] p_rect 另一矩形
         * @return true 当 p_rect 完全在此矩形内部
         */
        ARHUD_ALWAYS_INLINE bool Encloses(const Rect2 &p_rect) const
        {
            return (p_rect.position.x >= position.x) && (p_rect.position.y >= position.y) &&
                   ((p_rect.position.x + p_rect.size.x) <= (position.x + size.x)) &&
                   ((p_rect.position.y + p_rect.size.y) <= (position.y + size.y));
        }

        /**
         * @brief 判断点是否在矩形内
         * @param[in] p_point 目标点
         * @return true 当点在矩形内部（含边界）
         */
        ARHUD_ALWAYS_INLINE bool HasPoint(const Vec2 &p_point) const
        {
            if (p_point.x < position.x)
            {
                return false;
            }
            if (p_point.y < position.y)
            {
                return false;
            }
            if (p_point.x >= (position.x + size.x))
            {
                return false;
            }
            if (p_point.y >= (position.y + size.y))
            {
                return false;
            }
            return true;
        }

        /**
         * @brief 原地扩展以包含点
         * @param[in] p_point 目标点
         * @details 若点在外，则扩大 size 以包含该点
         */
        ARHUD_ALWAYS_INLINE void ExpandTo(const Vec2 &p_point)
        {
            Vec2 end = GetEnd();
            if (p_point.x < position.x)
            {
                position.x = p_point.x;
            }
            if (p_point.y < position.y)
            {
                position.y = p_point.y;
            }
            if (p_point.x > end.x)
            {
                end.x = p_point.x;
            }
            if (p_point.y > end.y)
            {
                end.y = p_point.y;
            }
            size = end - position;
        }

        /**
         * @brief 返回包含点的扩展矩形
         * @param[in] p_point 目标点
         * @return 新的扩展矩形
         */
        ARHUD_ALWAYS_INLINE Rect2 ExpandedTo(const Vec2 &p_point) const
        {
            Rect2 r = *this;
            r.ExpandTo(p_point);
            return r;
        }

        /**
         * @brief 获取与另一矩形的并集
         * @param[in] p_rect 另一矩形
         * @return 包含两个矩形的最小矩形
         */
        ARHUD_ALWAYS_INLINE Rect2 Merged(const Rect2 &p_rect) const
        {
            Rect2 r;
            r.position.x = MathFuncs::Min(position.x, p_rect.position.x);
            r.position.y = MathFuncs::Min(position.y, p_rect.position.y);
            Vec2 end_a = GetEnd();
            Vec2 end_b = p_rect.GetEnd();
            Vec2 end;
            end.x = MathFuncs::Max(end_a.x, end_b.x);
            end.y = MathFuncs::Max(end_a.y, end_b.y);
            r.size = end - r.position;
            return r;
        }

        /**
         * @brief 等距扩展矩形
         * @param[in] p_amount 扩展量
         * @return 各方向扩展 p_amount 的新矩形
         */
        ARHUD_ALWAYS_INLINE Rect2 Grow(float32 p_amount) const
        {
            return Rect2(position.x - p_amount, position.y - p_amount,
                         size.x + p_amount * 2.0f, size.y + p_amount * 2.0f);
        }

        /**
         * @brief 非等距扩展矩形
         * @param[in] p_left 左侧扩展量
         * @param[in] p_top 顶部扩展量
         * @param[in] p_right 右侧扩展量
         * @param[in] p_bottom 底部扩展量
         * @return 各方向分别扩展的新矩形
         */
        ARHUD_ALWAYS_INLINE Rect2 GrowIndividual(float32 p_left, float32 p_top,
                                                 float32 p_right, float32 p_bottom) const
        {
            return Rect2(position.x - p_left, position.y - p_top,
                         size.x + p_left + p_right, size.y + p_top + p_bottom);
        }

        /**
         * @brief 近似相等判断
         * @param[in] p_rect 比较矩形
         * @return true 当 position 和 size 均近似相等
         */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(const Rect2 &p_rect) const
        {
            return position.IsEqualApprox(p_rect.position) && size.IsEqualApprox(p_rect.size);
        }

        /**
         * @brief 相等比较
         */
        constexpr bool operator==(const Rect2 &p_rect) const { return position == p_rect.position && size == p_rect.size; }

        /**
         * @brief 不等比较
         */
        constexpr bool operator!=(const Rect2 &p_rect) const { return position != p_rect.position || size != p_rect.size; }

        /**
         * @brief 默认构造
         */
        constexpr Rect2() = default;

        /**
         * @brief 从分量构造
         * @param[in] p_x 左上角 X
         * @param[in] p_y 左上角 Y
         * @param[in] p_w 宽度
         * @param[in] p_h 高度
         */
        constexpr Rect2(float32 p_x, float32 p_y, float32 p_w, float32 p_h)
            : position(p_x, p_y), size(p_w, p_h) {}

        /**
         * @brief 从位置和尺寸构造
         * @param[in] p_pos 左上角位置
         * @param[in] p_size 尺寸
         */
        constexpr Rect2(const Vec2 &p_pos, const Vec2 &p_size) : position(p_pos), size(p_size) {}
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Rect2i — 2D 整数矩形
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 2D 整数矩形
     *
     * 用于像素级裁剪区域、纹理区域、瓦片索引等整数矩形场景。
     */
    struct Rect2i
    {
        /** @brief 左上角位置（像素坐标） */
        Vec2i position;
        /** @brief 尺寸（像素单位） */
        Size2i size;

        /**
         * @brief 获取矩形中心点
         * @return 中心点坐标
         */
        ARHUD_ALWAYS_INLINE Vec2i GetCenter() const
        {
            return Vec2i(position.x + size.width / 2, position.y + size.height / 2);
        }

        /**
         * @brief 获取矩形面积
         * @return width * height
         */
        ARHUD_ALWAYS_INLINE int32 GetArea() const { return size.Area(); }

        /**
         * @brief 判断是否有有效面积
         * @return true 当 width > 0 且 height > 0
         */
        ARHUD_ALWAYS_INLINE bool HasArea() const { return size.HasArea(); }

        /**
         * @brief 获取右下角点
         * @return position + size
         */
        ARHUD_ALWAYS_INLINE Vec2i GetEnd() const { return Vec2i(position.x + size.width, position.y + size.height); }

        /**
         * @brief 判断是否与另一矩形相交
         * @param[in] p_rect 另一矩形
         * @return true 当存在重叠区域
         */
        ARHUD_ALWAYS_INLINE bool Intersects(const Rect2i &p_rect) const
        {
            if (position.x >= (p_rect.position.x + p_rect.size.width))
            {
                return false;
            }
            if ((position.x + size.width) <= p_rect.position.x)
            {
                return false;
            }
            if (position.y >= (p_rect.position.y + p_rect.size.height))
            {
                return false;
            }
            if ((position.y + size.height) <= p_rect.position.y)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief 判断是否完全包含另一矩形
         * @param[in] p_rect 另一矩形
         * @return true 当 p_rect 完全在此矩形内部
         */
        ARHUD_ALWAYS_INLINE bool Encloses(const Rect2i &p_rect) const
        {
            return (p_rect.position.x >= position.x) && (p_rect.position.y >= position.y) &&
                   ((p_rect.position.x + p_rect.size.width) <= (position.x + size.width)) &&
                   ((p_rect.position.y + p_rect.size.height) <= (position.y + size.height));
        }

        /**
         * @brief 判断点是否在矩形内
         * @param[in] p_point 目标点
         * @return true 当点在矩形内部（含边界）
         */
        ARHUD_ALWAYS_INLINE bool HasPoint(const Vec2i &p_point) const
        {
            if (p_point.x < position.x)
            {
                return false;
            }
            if (p_point.y < position.y)
            {
                return false;
            }
            if (p_point.x >= (position.x + size.width))
            {
                return false;
            }
            if (p_point.y >= (position.y + size.height))
            {
                return false;
            }
            return true;
        }

        /**
         * @brief 获取与另一矩形的并集
         * @param[in] p_rect 另一矩形
         * @return 包含两个矩形的最小矩形
         */
        ARHUD_ALWAYS_INLINE Rect2i Merged(const Rect2i &p_rect) const
        {
            Rect2i r;
            r.position.x = MathFuncs::Min(position.x, p_rect.position.x);
            r.position.y = MathFuncs::Min(position.y, p_rect.position.y);
            Vec2i end_a = GetEnd();
            Vec2i end_b = p_rect.GetEnd();
            r.size.width = MathFuncs::Max(end_a.x, end_b.x) - r.position.x;
            r.size.height = MathFuncs::Max(end_a.y, end_b.y) - r.position.y;
            return r;
        }

        /**
         * @brief 相等比较
         */
        constexpr bool operator==(const Rect2i &p_rect) const { return position == p_rect.position && size == p_rect.size; }

        /**
         * @brief 不等比较
         */
        constexpr bool operator!=(const Rect2i &p_rect) const { return position != p_rect.position || size != p_rect.size; }

        /**
         * @brief 默认构造
         */
        constexpr Rect2i() = default;

        /**
         * @brief 从分量构造
         * @param[in] p_x 左上角 X
         * @param[in] p_y 左上角 Y
         * @param[in] p_w 宽度
         * @param[in] p_h 高度
         */
        constexpr Rect2i(int32 p_x, int32 p_y, int32 p_w, int32 p_h)
            : position(p_x, p_y), size(p_w, p_h) {}

        /**
         * @brief 从位置和尺寸构造
         * @param[in] p_pos 左上角位置
         * @param[in] p_size 尺寸
         */
        constexpr Rect2i(const Vec2i &p_pos, const Size2i &p_size) : position(p_pos), size(p_size) {}
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Color — RGBA 浮点颜色
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief RGBA 浮点颜色
     *
     * 每个分量范围 [0, 1]，支持混合、插值、HSV 转换等操作。
     * 用于 HUD 元素着色、Clear Color、Tint 等。
     */
    struct Color
    {
        /** @brief 红色 (1, 0, 0, 1) */
        static const Color kRed;
        /** @brief 绿色 (0, 1, 0, 1) */
        static const Color kGreen;
        /** @brief 蓝色 (0, 0, 1, 1) */
        static const Color kBlue;
        /** @brief 黄色 (1, 1, 0, 1) */
        static const Color kYellow;
        /** @brief 青色 (0, 1, 1, 1) */
        static const Color kCyan;
        /** @brief 洋红色 (1, 0, 1, 1) */
        static const Color kMagenta;
        /** @brief 白色 (1, 1, 1, 1) */
        static const Color kWhite;
        /** @brief 黑色 (0, 0, 0, 1) */
        static const Color kBlack;
        /** @brief 完全透明 (0, 0, 0, 0) */
        static const Color kTransparent;

        union
        {
            struct
            {
                float32 r; /**< 红色分量 [0, 1] */
                float32 g; /**< 绿色分量 [0, 1] */
                float32 b; /**< 蓝色分量 [0, 1] */
                float32 a; /**< Alpha 分量 [0, 1] */
            };
            float32 components[4]; /**< 分量数组访问 */
        };

        /**
         * @brief 按索引访问分量
         * @param[in] p_idx 分量索引 (0=r, 1=g, 2=b, 3=a)
         * @return 分量引用
         */
        ARHUD_ALWAYS_INLINE float32 &operator[](int32 p_idx) { return components[p_idx]; }

        /**
         * @brief 按索引访问分量（const 版本）
         * @param[in] p_idx 分量索引 (0=r, 1=g, 2=b, 3=a)
         * @return 分量常量引用
         */
        ARHUD_ALWAYS_INLINE const float32 &operator[](int32 p_idx) const { return components[p_idx]; }

        /**
         * @brief 转换为 RGBA8888 格式
         * @return 32 位整数，字节序为 R|G|B|A
         */
        ARHUD_ALWAYS_INLINE uint32 ToRGBA32() const
        {
            return (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(r * 255.0f), 0.0f, 255.0f)) << 24) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(g * 255.0f), 0.0f, 255.0f)) << 16) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(b * 255.0f), 0.0f, 255.0f)) << 8) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(a * 255.0f), 0.0f, 255.0f)));
        }

        /**
         * @brief 转换为 ARGB8888 格式
         * @return 32 位整数，字节序为 A|R|G|B（Windows GDI 格式）
         */
        ARHUD_ALWAYS_INLINE uint32 ToARGB32() const
        {
            return (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(a * 255.0f), 0.0f, 255.0f)) << 24) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(r * 255.0f), 0.0f, 255.0f)) << 16) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(g * 255.0f), 0.0f, 255.0f)) << 8) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(b * 255.0f), 0.0f, 255.0f)));
        }

        /**
         * @brief 转换为 ABGR8888 格式
         * @return 32 位整数，字节序为 A|B|G|R
         */
        ARHUD_ALWAYS_INLINE uint32 ToABGR32() const
        {
            return (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(a * 255.0f), 0.0f, 255.0f)) << 24) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(b * 255.0f), 0.0f, 255.0f)) << 16) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(g * 255.0f), 0.0f, 255.0f)) << 8) |
                   (static_cast<uint32>(MathFuncs::Clamp(MathFuncs::Round(r * 255.0f), 0.0f, 255.0f)));
        }

        /**
         * @brief 从 RGBA8888 创建颜色
         * @param[in] p_rgba 32 位整数，字节序为 R|G|B|A
         * @return 转换后的 Color
         */
        static ARHUD_ALWAYS_INLINE Color FromRGBA32(uint32 p_rgba)
        {
            Color c;
            c.r = static_cast<float32>((p_rgba >> 24) & 0xFF) / 255.0f;
            c.g = static_cast<float32>((p_rgba >> 16) & 0xFF) / 255.0f;
            c.b = static_cast<float32>((p_rgba >> 8) & 0xFF) / 255.0f;
            c.a = static_cast<float32>(p_rgba & 0xFF) / 255.0f;
            return c;
        }

        /**
         * @brief 从 ARGB8888 创建颜色
         * @param[in] p_argb 32 位整数，字节序为 A|R|G|B
         * @return 转换后的 Color
         */
        static ARHUD_ALWAYS_INLINE Color FromARGB32(uint32 p_argb)
        {
            Color c;
            c.a = static_cast<float32>((p_argb >> 24) & 0xFF) / 255.0f;
            c.r = static_cast<float32>((p_argb >> 16) & 0xFF) / 255.0f;
            c.g = static_cast<float32>((p_argb >> 8) & 0xFF) / 255.0f;
            c.b = static_cast<float32>(p_argb & 0xFF) / 255.0f;
            return c;
        }

        /**
         * @brief 从 HSV 创建颜色
         * @param[in] p_h 色相 [0, 1]
         * @param[in] p_s 饱和度 [0, 1]
         * @param[in] p_v 明度 [0, 1]
         * @param[in] p_a Alpha 分量（默认 1.0）
         * @return 转换后的 Color
         */
        static ARHUD_ALWAYS_INLINE Color FromHSV(float32 p_h, float32 p_s, float32 p_v,
                                                 float32 p_a = 1.0f)
        {
            Color c;
            c.SetHSV(p_h, p_s, p_v, p_a);
            return c;
        }

        /**
         * @brief 获取色相分量
         * @return 色相值 [0, 1]
         */
        ARHUD_ALWAYS_INLINE float32 GetH() const
        {
            float32 min_v = MathFuncs::Min(MathFuncs::Min(r, g), b);
            float32 max_v = MathFuncs::Max(MathFuncs::Max(r, g), b);
            float32 delta = max_v - min_v;
            if (delta < MathFuncs::kEpsilon)
            {
                return 0.0f;
            }
            float32 h;
            if (r == max_v)
            {
                h = (g - b) / delta;
            }
            else if (g == max_v)
            {
                h = 2.0f + (b - r) / delta;
            }
            else
            {
                h = 4.0f + (r - g) / delta;
            }
            h /= 6.0f;
            if (h < 0.0f)
            {
                h += 1.0f;
            }
            return h;
        }

        /**
         * @brief 获取饱和度分量
         * @return 饱和度值 [0, 1]
         */
        ARHUD_ALWAYS_INLINE float32 GetS() const
        {
            float32 min_v = MathFuncs::Min(MathFuncs::Min(r, g), b);
            float32 max_v = MathFuncs::Max(MathFuncs::Max(r, g), b);
            float32 delta = max_v - min_v;
            return max_v > MathFuncs::kEpsilon ? (delta / max_v) : 0.0f;
        }

        /**
         * @brief 获取明度分量
         * @return 明度值 [0, 1]
         */
        ARHUD_ALWAYS_INLINE float32 GetV() const { return MathFuncs::Max(MathFuncs::Max(r, g), b); }

        /**
         * @brief 设置 HSV 分量
         * @param[in] p_h 色相 [0, 1]
         * @param[in] p_s 饱和度 [0, 1]
         * @param[in] p_v 明度 [0, 1]
         * @param[in] p_a Alpha 分量（默认 1.0）
         */
        ARHUD_ALWAYS_INLINE void SetHSV(float32 p_h, float32 p_s, float32 p_v, float32 p_a = 1.0f)
        {
            a = p_a;
            if (p_s < MathFuncs::kEpsilon)
            {
                r = g = b = p_v;
                return;
            }
            float32 h = p_h * 6.0f;
            h = MathFuncs::Fposmod(h, 6.0f);
            int32 i = static_cast<int32>(MathFuncs::Floor(h));
            float32 f = h - static_cast<float32>(i);
            float32 p = p_v * (1.0f - p_s);
            float32 q = p_v * (1.0f - p_s * f);
            float32 t = p_v * (1.0f - p_s * (1.0f - f));
            switch (i)
            {
            case 0:
                r = p_v;
                g = t;
                b = p;
                break;
            case 1:
                r = q;
                g = p_v;
                b = p;
                break;
            case 2:
                r = p;
                g = p_v;
                b = t;
                break;
            case 3:
                r = p;
                g = q;
                b = p_v;
                break;
            case 4:
                r = t;
                g = p;
                b = p_v;
                break;
            default:
                r = p_v;
                g = p;
                b = q;
                break;
            }
        }

        /**
         * @brief 计算亮度
         * @return 加权亮度值（ITU-R BT.709）
         */
        ARHUD_ALWAYS_INLINE float32 GetLuminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

        /**
         * @brief 线性插值
         * @param[in] p_to 目标颜色
         * @param[in] p_weight 权重 [0, 1]
         * @return 插值后的颜色
         */
        ARHUD_ALWAYS_INLINE Color Lerp(const Color &p_to, float32 p_weight) const
        {
            return Color(MathFuncs::Lerp(r, p_to.r, p_weight), MathFuncs::Lerp(g, p_to.g, p_weight),
                         MathFuncs::Lerp(b, p_to.b, p_weight), MathFuncs::Lerp(a, p_to.a, p_weight));
        }

        /**
         * @brief 变暗
         * @param[in] p_amount 变暗量 [0, 1]
         * @return 变暗后的颜色
         */
        ARHUD_ALWAYS_INLINE Color Darkened(float32 p_amount) const
        {
            return Color(r * (1.0f - p_amount), g * (1.0f - p_amount), b * (1.0f - p_amount), a);
        }

        /**
         * @brief 变亮
         * @param[in] p_amount 变亮量 [0, 1]
         * @return 变亮后的颜色
         */
        ARHUD_ALWAYS_INLINE Color Lightened(float32 p_amount) const
        {
            return Color(r + (1.0f - r) * p_amount, g + (1.0f - g) * p_amount,
                         b + (1.0f - b) * p_amount, a);
        }

        /**
         * @brief 反转颜色（保留 Alpha）
         * @return 反转后的颜色
         */
        ARHUD_ALWAYS_INLINE Color Inverted() const { return Color(1.0f - r, 1.0f - g, 1.0f - b, a); }

        /**
         * @brief 设置 Alpha
         * @param[in] p_a 新的 Alpha 值
         * @return 保留 RGB 仅改变 Alpha 的颜色
         */
        ARHUD_ALWAYS_INLINE Color WithAlpha(float32 p_a) const { return Color(r, g, b, p_a); }

        /**
         * @brief 近似相等判断
         * @param[in] p_c 比较对象
         * @return true 当所有分量近似相等
         */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(const Color &p_c) const
        {
            return MathFuncs::IsEqualApprox(r, p_c.r) && MathFuncs::IsEqualApprox(g, p_c.g) &&
                   MathFuncs::IsEqualApprox(b, p_c.b) && MathFuncs::IsEqualApprox(a, p_c.a);
        }

        /**
         * @brief 颜色加法（分量相加）
         */
        constexpr Color operator+(const Color &p_c) const { return Color(r + p_c.r, g + p_c.g, b + p_c.b, a + p_c.a); }

        /**
         * @brief 颜色减法（分量相减）
         */
        constexpr Color operator-(const Color &p_c) const { return Color(r - p_c.r, g - p_c.g, b - p_c.b, a - p_c.a); }

        /**
         * @brief 取反（1 - 原分量）
         */
        constexpr Color operator-() const { return Color(1.0f - r, 1.0f - g, 1.0f - b, 1.0f - a); }

        /**
         * @brief 颜色乘法（分量相乘）
         */
        constexpr Color operator*(const Color &p_c) const { return Color(r * p_c.r, g * p_c.g, b * p_c.b, a * p_c.a); }

        /**
         * @brief 标量乘法
         */
        constexpr Color operator*(float32 p_s) const { return Color(r * p_s, g * p_s, b * p_s, a * p_s); }

        /**
         * @brief 颜色除法（分量相除）
         */
        constexpr Color operator/(const Color &p_c) const { return Color(r / p_c.r, g / p_c.g, b / p_c.b, a / p_c.a); }

        /**
         * @brief 标量除法
         */
        constexpr Color operator/(float32 p_s) const { return Color(r / p_s, g / p_s, b / p_s, a / p_s); }

        /**
         * @brief 加法赋值
         */
        constexpr Color &operator+=(const Color &p_c)
        {
            r += p_c.r;
            g += p_c.g;
            b += p_c.b;
            a += p_c.a;
            return *this;
        }

        /**
         * @brief 减法赋值
         */
        constexpr Color &operator-=(const Color &p_c)
        {
            r -= p_c.r;
            g -= p_c.g;
            b -= p_c.b;
            a -= p_c.a;
            return *this;
        }

        /**
         * @brief 乘法赋值（颜色）
         */
        constexpr Color &operator*=(const Color &p_c)
        {
            r *= p_c.r;
            g *= p_c.g;
            b *= p_c.b;
            a *= p_c.a;
            return *this;
        }

        /**
         * @brief 乘法赋值（标量）
         */
        constexpr Color &operator*=(float32 p_s)
        {
            r *= p_s;
            g *= p_s;
            b *= p_s;
            a *= p_s;
            return *this;
        }

        /**
         * @brief 除法赋值
         */
        constexpr Color &operator/=(float32 p_s)
        {
            r /= p_s;
            g /= p_s;
            b /= p_s;
            a /= p_s;
            return *this;
        }

        /**
         * @brief 相等比较
         */
        constexpr bool operator==(const Color &p_c) const { return r == p_c.r && g == p_c.g && b == p_c.b && a == p_c.a; }

        /**
         * @brief 不等比较
         */
        constexpr bool operator!=(const Color &p_c) const { return r != p_c.r || g != p_c.g || b != p_c.b || a != p_c.a; }

        /**
         * @brief 默认构造（黑色不透明）
         */
        constexpr Color() : r(0.0f), g(0.0f), b(0.0f), a(1.0f) {}

        /**
         * @brief 从 RGBA 分量构造
         * @param[in] p_r 红色分量
         * @param[in] p_g 绿色分量
         * @param[in] p_b 蓝色分量
         * @param[in] p_a Alpha 分量（默认 1.0）
         */
        constexpr Color(float32 p_r, float32 p_g, float32 p_b, float32 p_a = 1.0f)
            : r(p_r), g(p_g), b(p_b), a(p_a) {}

        /**
         * @brief 从 RGBA8888 整数构造
         * @param[in] p_rgba32 32 位整数，字节序为 R|G|B|A
         */
        Color(uint32 p_rgba32) { *this = FromRGBA32(p_rgba32); }
    };

    inline constexpr Color Color::kRed = Color(1.0f, 0.0f, 0.0f);
    inline constexpr Color Color::kGreen = Color(0.0f, 1.0f, 0.0f);
    inline constexpr Color Color::kBlue = Color(0.0f, 0.0f, 1.0f);
    inline constexpr Color Color::kYellow = Color(1.0f, 1.0f, 0.0f);
    inline constexpr Color Color::kCyan = Color(0.0f, 1.0f, 1.0f);
    inline constexpr Color Color::kMagenta = Color(1.0f, 0.0f, 1.0f);
    inline constexpr Color Color::kWhite = Color(1.0f, 1.0f, 1.0f);
    inline constexpr Color Color::kBlack = Color(0.0f, 0.0f, 0.0f);
    inline constexpr Color Color::kTransparent = Color(0.0f, 0.0f, 0.0f, 0.0f);

    constexpr Color operator*(float32 p_s, const Color &p_c) { return p_c * p_s; }

    // ═══════════════════════════════════════════════════════════════════════
    // Transform2D — 2D 仿射变换（2×3 列主序矩阵）
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 2D 仿射变换（2×3 列主序矩阵）
     *
     * 内存布局：
     *   columns[0] = X 轴基向量（旋转+缩放）
     *   columns[1] = Y 轴基向量（旋转+缩放）
     *   columns[2] = 平移向量
     *
     * 数学表示：
     *   | columns[0].x  columns[1].x  columns[2].x |
     *   | columns[0].y  columns[1].y  columns[2].y |
     *   |     0              0              1       |
     *
     * 用于 HUD 元素的平移、旋转、缩放变换。
     */
    struct Transform2D
    {
        /** @brief 三列向量（X轴基向量，Y轴基向量，平移） */
        Vec2 columns[3] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}};

        /**
         * @brief 计算 X 轴投影权重
         * @param[in] p_with 目标向量
         * @return columns[0] 与 p_with 的点积
         */
        ARHUD_ALWAYS_INLINE float32 BasisDotX(const Vec2 &p_with) const
        {
            return columns[0].x * p_with.x + columns[1].x * p_with.y;
        }

        /**
         * @brief 计算 Y 轴投影权重
         * @param[in] p_with 目标向量
         * @return columns[1] 与 p_with 的点积
         */
        ARHUD_ALWAYS_INLINE float32 BasisDotY(const Vec2 &p_with) const
        {
            return columns[0].y * p_with.x + columns[1].y * p_with.y;
        }

        /**
         * @brief 原地求逆
         * @details 适用于非奇异矩阵，奇异时触发断言
         */
        ARHUD_ALWAYS_INLINE void Invert()
        {
            float32 det = columns[0].x * columns[1].y - columns[0].y * columns[1].x;
            ARHUD_ASSERT(!MathFuncs::IsZeroApprox(det));
            float32 inv_det = 1.0f / det;

            Vec2 x = columns[0];
            columns[0].x = columns[1].y * inv_det;
            columns[0].y = -x.y * inv_det;
            columns[1].x = -columns[1].x * inv_det;
            columns[1].y = x.x * inv_det;

            Vec2 t = columns[2];
            columns[2].x = -columns[0].x * t.x - columns[1].x * t.y;
            columns[2].y = -columns[0].y * t.x - columns[1].y * t.y;
        }

        /**
         * @brief 返回逆变换
         * @return 新的逆变换矩阵
         */
        ARHUD_ALWAYS_INLINE Transform2D Inverted() const
        {
            Transform2D res = *this;
            res.Invert();
            return res;
        }

        /**
         * @brief 原地仿射求逆（不做齐次坐标归一化）
         * @details 用于仅涉及旋转、平移、缩放的变换求逆
         */
        ARHUD_ALWAYS_INLINE void AffineInvert()
        {
            float32 det = columns[0].x * columns[1].y - columns[0].y * columns[1].x;
            float32 inv_det = 1.0f / det;

            Vec2 x = columns[0];
            columns[0].x = columns[1].y * inv_det;
            columns[0].y = -x.y * inv_det;
            columns[1].x = -columns[1].x * inv_det;
            columns[1].y = x.x * inv_det;

            Vec2 t = columns[2];
            columns[2].x = -columns[0].x * t.x - columns[1].x * t.y;
            columns[2].y = -columns[0].y * t.x - columns[1].y * t.y;
        }

        /**
         * @brief 返回仿射逆变换
         * @return 新的仿射逆变换矩阵
         */
        ARHUD_ALWAYS_INLINE Transform2D AffineInverted() const
        {
            Transform2D res = *this;
            res.AffineInvert();
            return res;
        }

        /**
         * @brief 变换向量（含平移）
         * @param[in] p_vec 输入向量
         * @return 变换后的向量
         */
        ARHUD_ALWAYS_INLINE Vec2 Xform(const Vec2 &p_vec) const
        {
            return Vec2(BasisDotX(p_vec) + columns[2].x, BasisDotY(p_vec) + columns[2].y);
        }

        /**
         * @brief 仅用基向量变换向量（不含平移）
         * @param[in] p_vec 输入向量
         * @return 仅经旋转/缩放变换的向量
         */
        ARHUD_ALWAYS_INLINE Vec2 BasisXform(const Vec2 &p_vec) const
        {
            return Vec2(BasisDotX(p_vec), BasisDotY(p_vec));
        }

        /**
         * @brief 逆变换向量
         * @param[in] p_vec 输入向量
         * @return 逆变换后的向量
         */
        ARHUD_ALWAYS_INLINE Vec2 XformInv(const Vec2 &p_vec) const
        {
            Vec2 v = p_vec - columns[2];
            return Vec2(columns[0].x * v.x + columns[0].y * v.y,
                        columns[1].x * v.x + columns[1].y * v.y);
        }

        /**
         * @brief 变换矩形
         * @param[in] p_rect 输入矩形
         * @return 变换后的矩形（可能不再是轴对齐）
         */
        ARHUD_ALWAYS_INLINE Rect2 Xform(const Rect2 &p_rect) const
        {
            Vec2 x = BasisXform(Vec2(p_rect.size.x, 0.0f));
            Vec2 y = BasisXform(Vec2(0.0f, p_rect.size.y));
            Vec2 pos = Xform(p_rect.position);
            Rect2 r;
            r.position = pos;
            r.size.width = 0.0f;
            r.size.height = 0.0f;
            r = r.ExpandedTo(pos + x);
            r = r.ExpandedTo(pos + y);
            r = r.ExpandedTo(pos + x + y);
            return r;
        }

        /**
         * @brief 设置旋转变换
         * @param[in] p_rotation 弧度旋转角度
         */
        ARHUD_ALWAYS_INLINE void SetRotation(float32 p_rotation)
        {
            float32 cs = MathFuncs::Cos(p_rotation);
            float32 sn = MathFuncs::Sin(p_rotation);
            columns[0].x = cs;
            columns[0].y = sn;
            columns[1].x = -sn;
            columns[1].y = cs;
        }

        /**
         * @brief 获取旋转变换角度
         * @return 弧度表示的旋转角度
         */
        ARHUD_ALWAYS_INLINE float32 Rotation() const
        {
            return MathFuncs::Atan2(columns[0].y, columns[0].x);
        }

        /**
         * @brief 同时设置旋转和缩放
         * @param[in] p_rotation 弧度旋转角度
         * @param[in] p_scale 各轴缩放因子
         */
        ARHUD_ALWAYS_INLINE void SetRotationAndScale(float32 p_rotation, const Vec2 &p_scale)
        {
            float32 cs = MathFuncs::Cos(p_rotation);
            float32 sn = MathFuncs::Sin(p_rotation);
            columns[0].x = cs * p_scale.x;
            columns[0].y = sn * p_scale.x;
            columns[1].x = -sn * p_scale.y;
            columns[1].y = cs * p_scale.y;
        }

        /**
         * @brief 获取缩放因子
         * @return 各轴缩放因子向量
         */
        ARHUD_ALWAYS_INLINE Vec2 Scale() const
        {
            float32 det_sign = MathFuncs::Sign(columns[0].x * columns[1].y - columns[0].y * columns[1].x);
            return Vec2(Vec2(columns[0].x, columns[0].y).Length(),
                        det_sign * Vec2(columns[1].x, columns[1].y).Length());
        }

        /**
         * @brief 设置缩放因子
         * @param[in] p_scale 各轴缩放因子
         */
        ARHUD_ALWAYS_INLINE void SetScale(const Vec2 &p_scale)
        {
            columns[0].Normalize();
            columns[1].Normalize();
            columns[0] *= p_scale.x;
            columns[1] *= p_scale.y;
        }

        /**
         * @brief 获取斜切角
         * @return 弧度表示的斜切角
         */
        ARHUD_ALWAYS_INLINE float32 Skew() const
        {
            Vec2 basis0(columns[0].x, columns[0].y);
            Vec2 basis1(columns[1].x, columns[1].y);
            basis0.Normalize();
            basis1.Normalize();
            return MathFuncs::Atan2(basis0.Dot(basis1), basis0.Cross(basis1)) - MathFuncs::kPi / 4.0f;
        }

        /**
         * @brief 设置斜切角
         * @param[in] p_skew 弧度表示的斜切角
         */
        ARHUD_ALWAYS_INLINE void SetSkew(float32 p_skew)
        {
            float32 rotation = Rotation();
            Vec2 scale = Scale();
            float32 cs = MathFuncs::Cos(rotation);
            float32 sn = MathFuncs::Sin(rotation);
            columns[0].x = cs * scale.x;
            columns[0].y = sn * scale.x;
            columns[1].x = (-sn + MathFuncs::Tan(p_skew) * cs) * scale.y;
            columns[1].y = (cs + MathFuncs::Tan(p_skew) * sn) * scale.y;
        }

        /**
         * @brief 原地正交化
         * @details Gram-Schmidt 正交化，使基向量两两垂直
         */
        ARHUD_ALWAYS_INLINE void Orthonormalize()
        {
            Vec2 x = columns[0];
            Vec2 y = columns[1];
            x.Normalize();
            y = y - x * x.Dot(y);
            y.Normalize();
            columns[0] = x;
            columns[1] = y;
        }

        /**
         * @brief 返回正交化后的变换
         * @return 新的正交化矩阵
         */
        ARHUD_ALWAYS_INLINE Transform2D Orthonormalized() const
        {
            Transform2D res = *this;
            res.Orthonormalize();
            return res;
        }

        /**
         * @brief 近似相等判断
         * @param[in] p_t 比较对象
         * @return true 当所有列向量近似相等
         */
        ARHUD_ALWAYS_INLINE bool IsEqualApprox(const Transform2D &p_t) const
        {
            return columns[0].IsEqualApprox(p_t.columns[0]) &&
                   columns[1].IsEqualApprox(p_t.columns[1]) &&
                   columns[2].IsEqualApprox(p_t.columns[2]);
        }

        /**
         * @brief 矩阵乘法（组合变换）
         * @param[in] p_t 右乘矩阵
         * @return 组合后的变换矩阵
         */
        Transform2D operator*(const Transform2D &p_t) const
        {
            Transform2D res;
            res.columns[0].x = BasisDotX(p_t.columns[0]);
            res.columns[0].y = BasisDotY(p_t.columns[0]);
            res.columns[1].x = BasisDotX(p_t.columns[1]);
            res.columns[1].y = BasisDotY(p_t.columns[1]);
            res.columns[2].x = BasisDotX(p_t.columns[2]) + columns[2].x;
            res.columns[2].y = BasisDotY(p_t.columns[2]) + columns[2].y;
            return res;
        }

        /**
         * @brief 标量乘法
         * @param[in] p_s 标量值
         * @return 缩放后的变换矩阵
         */
        Transform2D operator*(float32 p_s) const
        {
            Transform2D res = *this;
            res.columns[0] *= p_s;
            res.columns[1] *= p_s;
            res.columns[2] *= p_s;
            return res;
        }

        /**
         * @brief 相等比较
         */
        constexpr bool operator==(const Transform2D &p_t) const
        {
            return columns[0] == p_t.columns[0] && columns[1] == p_t.columns[1] &&
                   columns[2] == p_t.columns[2];
        }

        /**
         * @brief 不等比较
         */
        constexpr bool operator!=(const Transform2D &p_t) const
        {
            return columns[0] != p_t.columns[0] || columns[1] != p_t.columns[1] ||
                   columns[2] != p_t.columns[2];
        }

        /**
         * @brief 默认构造（单位矩阵）
         */
        constexpr Transform2D() = default;

        /**
         * @brief 从旋转和平移构造
         * @param[in] p_rotation 弧度旋转角度
         * @param[in] p_position 平移向量
         */
        Transform2D(float32 p_rotation, const Vec2 &p_position)
        {
            float32 cs = MathFuncs::Cos(p_rotation);
            float32 sn = MathFuncs::Sin(p_rotation);
            columns[0] = Vec2(cs, sn);
            columns[1] = Vec2(-sn, cs);
            columns[2] = p_position;
        }

        /**
         * @brief 从旋转、缩放、斜切和平移构造
         * @param[in] p_rotation 弧度旋转角度
         * @param[in] p_scale 各轴缩放因子
         * @param[in] p_skew 弧度斜切角
         * @param[in] p_position 平移向量
         */
        Transform2D(float32 p_rotation, const Vec2 &p_scale, float32 p_skew,
                    const Vec2 &p_position)
        {
            float32 cs = MathFuncs::Cos(p_rotation);
            float32 sn = MathFuncs::Sin(p_rotation);
            columns[0] = Vec2(cs * p_scale.x, sn * p_scale.x);
            columns[1] = Vec2((-sn + MathFuncs::Tan(p_skew) * cs) * p_scale.y,
                              (cs + MathFuncs::Tan(p_skew) * sn) * p_scale.y);
            columns[2] = p_position;
        }

        /**
         * @brief 从基向量和原点构造
         * @param[in] p_x X 轴基向量
         * @param[in] p_y Y 轴基向量
         * @param[in] p_origin 原点/平移向量
         */
        constexpr Transform2D(const Vec2 &p_x, const Vec2 &p_y, const Vec2 &p_origin)
            : columns{p_x, p_y, p_origin} {}

        /**
         * @brief 返回单位变换矩阵
         * @return Identity 矩阵
         */
        static ARHUD_ALWAYS_INLINE Transform2D Identity()
        {
            return Transform2D(Vec2(1.0f, 0.0f), Vec2(0.0f, 1.0f), Vec2(0.0f, 0.0f));
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Mat4 — 4×4 列主序矩阵
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 4×4 列主序矩阵
     *
     * 用于透视投影、视图变换、模型矩阵等 3D 变换。
     * 内存布局与 OpenGL 兼容（列主序）：
     *   elements[0..3]   = 第 0 列
     *   elements[4..7]   = 第 1 列
     *   elements[8..11]  = 第 2 列
     *   elements[12..15] = 第 3 列
     *
     * 数学表示：
     *   | e[0]  e[4]  e[8]   e[12] |
     *   | e[1]  e[5]  e[9]   e[13] |
     *   | e[2]  e[6]  e[10]  e[14] |
     *   | e[3]  e[7]  e[11]  e[15] |
     */
    struct Mat4
    {
        /** @brief 16 个元素的列主序数组 */
        float32 elements[16];

        /**
         * @brief 默认构造（单位矩阵）
         */
        ARHUD_ALWAYS_INLINE Mat4()
        {
            elements[0] = 1.0f;
            elements[4] = 0.0f;
            elements[8] = 0.0f;
            elements[12] = 0.0f;
            elements[1] = 0.0f;
            elements[5] = 1.0f;
            elements[9] = 0.0f;
            elements[13] = 0.0f;
            elements[2] = 0.0f;
            elements[6] = 0.0f;
            elements[10] = 1.0f;
            elements[14] = 0.0f;
            elements[3] = 0.0f;
            elements[7] = 0.0f;
            elements[11] = 0.0f;
            elements[15] = 1.0f;
        }

        /**
         * @brief 按 (列, 行) 访问元素
         * @param[in] p_col 列索引 [0, 3]
         * @param[in] p_row 行索引 [0, 3]
         * @return 元素引用
         */
        ARHUD_ALWAYS_INLINE float32 &operator()(int32 p_col, int32 p_row)
        {
            return elements[p_col * 4 + p_row];
        }

        /**
         * @brief 按 (列, 行) 访问元素（const 版本）
         * @param[in] p_col 列索引 [0, 3]
         * @param[in] p_row 行索引 [0, 3]
         * @return 元素常量引用
         */
        ARHUD_ALWAYS_INLINE const float32 &operator()(int32 p_col, int32 p_row) const
        {
            return elements[p_col * 4 + p_row];
        }

        /**
         * @brief 矩阵与 4D 向量乘法
         * @param[in] p_vec 输入 4D 向量
         * @return 变换后的 4D 向量
         */
        ARHUD_ALWAYS_INLINE Vec4 operator*(const Vec4 &p_vec) const
        {
            return Vec4(
                elements[0] * p_vec.x + elements[4] * p_vec.y + elements[8] * p_vec.z + elements[12] * p_vec.w,
                elements[1] * p_vec.x + elements[5] * p_vec.y + elements[9] * p_vec.z + elements[13] * p_vec.w,
                elements[2] * p_vec.x + elements[6] * p_vec.y + elements[10] * p_vec.z + elements[14] * p_vec.w,
                elements[3] * p_vec.x + elements[7] * p_vec.y + elements[11] * p_vec.z + elements[15] * p_vec.w);
        }

        /**
         * @brief 矩阵乘法
         * @param[in] p_m 右乘矩阵
         * @return 组合后的矩阵
         */
        Mat4 operator*(const Mat4 &p_m) const
        {
            Mat4 res;
            for (int32 i = 0; i < 4; ++i)
            {
                for (int32 j = 0; j < 4; ++j)
                {
                    res.elements[i * 4 + j] =
                        elements[0 * 4 + j] * p_m.elements[i * 4 + 0] +
                        elements[1 * 4 + j] * p_m.elements[i * 4 + 1] +
                        elements[2 * 4 + j] * p_m.elements[i * 4 + 2] +
                        elements[3 * 4 + j] * p_m.elements[i * 4 + 3];
                }
            }
            return res;
        }

        /**
         * @brief 矩阵乘法赋值
         * @param[in] p_m 右乘矩阵
         * @return 自身引用
         */
        Mat4 &operator*=(const Mat4 &p_m)
        {
            *this = *this * p_m;
            return *this;
        }

        /**
         * @brief 相等比较
         */
        constexpr bool operator==(const Mat4 &p_m) const
        {
            for (int32 i = 0; i < 16; ++i)
            {
                if (elements[i] != p_m.elements[i])
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 不等比较
         */
        constexpr bool operator!=(const Mat4 &p_m) const { return !(*this == p_m); }

        /**
         * @brief 计算行列式
         * @return 矩阵行列式值
         */
        ARHUD_ALWAYS_INLINE float32 Determinant() const
        {
            const float32 *e = elements;
            float32 det = 0.0f;
            det += e[0] * (e[5] * (e[10] * e[15] - e[14] * e[11]) -
                           e[9] * (e[6] * e[15] - e[14] * e[7]) +
                           e[13] * (e[6] * e[11] - e[10] * e[7]));
            det -= e[4] * (e[1] * (e[10] * e[15] - e[14] * e[11]) -
                           e[9] * (e[2] * e[15] - e[14] * e[3]) +
                           e[13] * (e[2] * e[11] - e[10] * e[3]));
            det += e[8] * (e[1] * (e[6] * e[15] - e[14] * e[7]) -
                           e[5] * (e[2] * e[15] - e[14] * e[3]) +
                           e[13] * (e[2] * e[7] - e[6] * e[3]));
            det -= e[12] * (e[1] * (e[6] * e[11] - e[10] * e[7]) -
                            e[5] * (e[2] * e[11] - e[10] * e[3]) +
                            e[9] * (e[2] * e[7] - e[6] * e[3]));
            return det;
        }

        /**
         * @brief 计算逆矩阵
         * @return 逆矩阵，若奇异则返回单位矩阵
         */
        Mat4 Inverse() const
        {
            Mat4 res;
            float32 *co = res.elements;
            const float32 *e = elements;

            co[0] = e[5] * e[10] * e[15] - e[5] * e[14] * e[11] - e[9] * e[6] * e[15] +
                    e[9] * e[14] * e[7] + e[13] * e[6] * e[11] - e[13] * e[10] * e[7];
            co[4] = -e[4] * e[10] * e[15] + e[4] * e[14] * e[11] + e[8] * e[6] * e[15] -
                    e[8] * e[14] * e[7] - e[12] * e[6] * e[11] + e[12] * e[10] * e[7];
            co[8] = e[4] * e[9] * e[15] - e[4] * e[13] * e[11] - e[8] * e[5] * e[15] +
                    e[8] * e[13] * e[7] + e[12] * e[5] * e[11] - e[12] * e[9] * e[7];
            co[12] = -e[4] * e[9] * e[14] + e[4] * e[13] * e[10] + e[8] * e[5] * e[14] -
                     e[8] * e[13] * e[6] - e[12] * e[5] * e[10] + e[12] * e[9] * e[6];

            co[1] = -e[1] * e[10] * e[15] + e[1] * e[14] * e[11] + e[9] * e[2] * e[15] -
                    e[9] * e[14] * e[3] - e[13] * e[2] * e[11] + e[13] * e[10] * e[3];
            co[5] = e[0] * e[10] * e[15] - e[0] * e[14] * e[11] - e[8] * e[2] * e[15] +
                    e[8] * e[14] * e[3] + e[12] * e[2] * e[11] - e[12] * e[10] * e[3];
            co[9] = -e[0] * e[9] * e[15] + e[0] * e[13] * e[11] + e[8] * e[1] * e[15] -
                    e[8] * e[13] * e[3] - e[12] * e[1] * e[11] + e[12] * e[9] * e[3];
            co[13] = e[0] * e[9] * e[14] - e[0] * e[13] * e[10] - e[8] * e[1] * e[14] +
                     e[8] * e[13] * e[2] + e[12] * e[1] * e[10] - e[12] * e[9] * e[2];

            co[2] = e[1] * e[6] * e[15] - e[1] * e[14] * e[7] - e[5] * e[2] * e[15] +
                    e[5] * e[14] * e[3] + e[13] * e[2] * e[7] - e[13] * e[6] * e[3];
            co[6] = -e[0] * e[6] * e[15] + e[0] * e[14] * e[7] + e[4] * e[2] * e[15] -
                    e[4] * e[14] * e[3] - e[12] * e[2] * e[7] + e[12] * e[6] * e[3];
            co[10] = e[0] * e[5] * e[15] - e[0] * e[13] * e[7] - e[4] * e[1] * e[15] +
                     e[4] * e[13] * e[3] + e[12] * e[1] * e[7] - e[12] * e[5] * e[3];
            co[14] = -e[0] * e[5] * e[14] + e[0] * e[13] * e[6] + e[4] * e[1] * e[14] -
                     e[4] * e[13] * e[2] - e[12] * e[1] * e[6] + e[12] * e[5] * e[2];

            co[3] = -e[1] * e[6] * e[11] + e[1] * e[10] * e[7] + e[5] * e[2] * e[11] -
                    e[5] * e[10] * e[3] - e[9] * e[2] * e[7] + e[9] * e[6] * e[3];
            co[7] = e[0] * e[6] * e[11] - e[0] * e[10] * e[7] - e[4] * e[2] * e[11] +
                    e[4] * e[10] * e[3] + e[8] * e[2] * e[7] - e[8] * e[6] * e[3];
            co[11] = -e[0] * e[5] * e[11] + e[0] * e[9] * e[7] + e[4] * e[1] * e[11] -
                     e[4] * e[9] * e[3] - e[8] * e[1] * e[7] + e[8] * e[5] * e[3];
            co[15] = e[0] * e[5] * e[10] - e[0] * e[9] * e[6] - e[4] * e[1] * e[10] +
                     e[4] * e[9] * e[2] + e[8] * e[1] * e[6] - e[8] * e[5] * e[2];

            float32 det = e[0] * co[0] + e[4] * co[1] + e[8] * co[2] + e[12] * co[3];
            if (MathFuncs::IsZeroApprox(det))
            {
                return Identity();
            }
            float32 inv_det = 1.0f / det;
            for (int32 i = 0; i < 16; ++i)
            {
                res.elements[i] *= inv_det;
            }
            return res;
        }

        /**
         * @brief 返回转置矩阵
         * @return 转置后的矩阵
         */
        ARHUD_ALWAYS_INLINE Mat4 Transposed() const
        {
            Mat4 res;
            res.elements[0] = elements[0];
            res.elements[1] = elements[4];
            res.elements[2] = elements[8];
            res.elements[3] = elements[12];
            res.elements[4] = elements[1];
            res.elements[5] = elements[5];
            res.elements[6] = elements[9];
            res.elements[7] = elements[13];
            res.elements[8] = elements[2];
            res.elements[9] = elements[6];
            res.elements[10] = elements[10];
            res.elements[11] = elements[14];
            res.elements[12] = elements[3];
            res.elements[13] = elements[7];
            res.elements[14] = elements[11];
            res.elements[15] = elements[15];
            return res;
        }

        /**
         * @brief 返回单位矩阵
         * @return 4×4 单位矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 Identity()
        {
            Mat4 m;
            return m;
        }

        /**
         * @brief 创建正交投影矩阵
         * @param[in] p_left 左侧裁剪平面
         * @param[in] p_right 右侧裁剪平面
         * @param[in] p_bottom 底部裁剪平面
         * @param[in] p_top 顶部裁剪平面
         * @param[in] p_near 近裁剪平面
         * @param[in] p_far 远裁剪平面
         * @return 正交投影矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 Orthographic(float32 p_left, float32 p_right,
                                                     float32 p_bottom, float32 p_top, float32 p_near, float32 p_far)
        {
            Mat4 m;
            m.elements[0] = 2.0f / (p_right - p_left);
            m.elements[5] = 2.0f / (p_top - p_bottom);
            m.elements[10] = -2.0f / (p_far - p_near);
            m.elements[12] = -(p_right + p_left) / (p_right - p_left);
            m.elements[13] = -(p_top + p_bottom) / (p_top - p_bottom);
            m.elements[14] = -(p_far + p_near) / (p_far - p_near);
            return m;
        }

        /**
         * @brief 创建透视投影矩阵
         * @param[in] p_fov_y 垂直视野角度（弧度）
         * @param[in] p_aspect 宽高比
         * @param[in] p_near 近裁剪平面
         * @param[in] p_far 远裁剪平面
         * @return 透视投影矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 Perspective(float32 p_fov_y, float32 p_aspect,
                                                    float32 p_near, float32 p_far)
        {
            Mat4 m;
            float32 tan_half_fov = MathFuncs::Tan(p_fov_y * 0.5f);
            m.elements[0] = 1.0f / (p_aspect * tan_half_fov);
            m.elements[5] = 1.0f / tan_half_fov;
            m.elements[10] = -(p_far + p_near) / (p_far - p_near);
            m.elements[11] = -1.0f;
            m.elements[14] = -(2.0f * p_far * p_near) / (p_far - p_near);
            m.elements[15] = 0.0f;
            return m;
        }

        /**
         * @brief 创建观察矩阵（LookAt）
         * @param[in] p_eye 摄像机位置
         * @param[in] p_target 观察目标位置
         * @param[in] p_up 摄像机上向量
         * @return 观察矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 LookAt(const Vec3 &p_eye, const Vec3 &p_target,
                                               const Vec3 &p_up)
        {
            Vec3 f = (p_target - p_eye).Normalized();
            Vec3 r = f.Cross(p_up).Normalized();
            Vec3 u = r.Cross(f);

            Mat4 m;
            m.elements[0] = r.x;
            m.elements[4] = r.y;
            m.elements[8] = r.z;
            m.elements[1] = u.x;
            m.elements[5] = u.y;
            m.elements[9] = u.z;
            m.elements[2] = -f.x;
            m.elements[6] = -f.y;
            m.elements[10] = -f.z;
            m.elements[12] = -r.Dot(p_eye);
            m.elements[13] = -u.Dot(p_eye);
            m.elements[14] = f.Dot(p_eye);
            return m;
        }

        /**
         * @brief 创建平移矩阵
         * @param[in] p_offset 平移向量
         * @return 平移矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 Translation(const Vec3 &p_offset)
        {
            Mat4 m;
            m.elements[12] = p_offset.x;
            m.elements[13] = p_offset.y;
            m.elements[14] = p_offset.z;
            return m;
        }

        /**
         * @brief 创建缩放矩阵
         * @param[in] p_scale 缩放向量
         * @return 缩放矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 Scaling(const Vec3 &p_scale)
        {
            Mat4 m;
            m.elements[0] = p_scale.x;
            m.elements[5] = p_scale.y;
            m.elements[10] = p_scale.z;
            return m;
        }

        /**
         * @brief 创建绕 X 轴旋转矩阵
         * @param[in] p_angle 旋转角度（弧度）
         * @return 旋转矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 RotationX(float32 p_angle)
        {
            Mat4 m;
            float32 cs = MathFuncs::Cos(p_angle);
            float32 sn = MathFuncs::Sin(p_angle);
            m.elements[5] = cs;
            m.elements[9] = -sn;
            m.elements[6] = sn;
            m.elements[10] = cs;
            return m;
        }

        /**
         * @brief 创建绕 Y 轴旋转矩阵
         * @param[in] p_angle 旋转角度（弧度）
         * @return 旋转矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 RotationY(float32 p_angle)
        {
            Mat4 m;
            float32 cs = MathFuncs::Cos(p_angle);
            float32 sn = MathFuncs::Sin(p_angle);
            m.elements[0] = cs;
            m.elements[8] = sn;
            m.elements[2] = -sn;
            m.elements[10] = cs;
            return m;
        }

        /**
         * @brief 创建绕 Z 轴旋转矩阵
         * @param[in] p_angle 旋转角度（弧度）
         * @return 旋转矩阵
         */
        static ARHUD_ALWAYS_INLINE Mat4 RotationZ(float32 p_angle)
        {
            Mat4 m;
            float32 cs = MathFuncs::Cos(p_angle);
            float32 sn = MathFuncs::Sin(p_angle);
            m.elements[0] = cs;
            m.elements[4] = -sn;
            m.elements[1] = sn;
            m.elements[5] = cs;
            return m;
        }
    };

} // namespace arhud