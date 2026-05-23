/**
 * @file arhud_string.h
 * @brief UTF-32 字符串类
 *
 * 参考 Godot 4.6 String 设计，内部使用 char32_t 存储以原生支持 Unicode。
 * 基于 LocalVector<char32_t> 而非 COW CowData，符合项目"禁止 COW 容器"规范。
 *
 * 设计取舍：
 *   - 内部编码 UTF-32（每字符固定 4 字节），牺牲内存换取 O(1) 索引访问
 *   - 与 C API 交互时转换为 UTF-8（Utf8() / CStr()）
 *   - 不实现 Godot 的全部字符串方法，仅保留 HUD 引擎所需核心功能
 *   - 格式化使用 Format() 静态方法，不支持 printf 风格可变参数
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <cstdarg>
#include <cstring>

#include "template/local_vector.h"
#include "typedefs.h"

namespace arhud
{

    /**
     * @brief UTF-32 字符串
     *
     * 内部以 char32_t 数组存储，末尾始终保留一个 U'\0' 终止符。
     * Length() 返回不含终止符的字符数，Size() 返回含终止符的总容量。
     *
     * 典型用途：
     * - 资源路径（shader_path, texture_path）
     * - 日志格式化消息
     * - HUD 文本元素
     * - 配置键值
     */
    class String
    {
    public:
        static constexpr int32_t kNpos = -1;

        // ═══════════════════════════════════════════════════════════════
        // 构造 / 析构 / 赋值
        // ═══════════════════════════════════════════════════════════════

        String() { data_.PushBack(U'\0'); }

        String(const String &p_other) = default;
        String(String &&p_other) noexcept = default;

        /**
         * @brief 从 UTF-8 C 字符串构造
         *
         * @param[in] p_utf8 UTF-8 编码的 C 字符串
         */
        String(const char *p_utf8);

        /**
         * @brief 从 UTF-8 C 字符串构造（指定长度）
         *
         * @param[in] p_utf8 UTF-8 编码的 C 字符串
         * @param[in] p_len  字节长度
         */
        String(const char *p_utf8, int32_t p_len);

        /**
         * @brief 从 char32_t C 字符串构造
         *
         * @param[in] p_utf32 UTF-32 编码的 C 字符串
         */
        String(const char32_t *p_utf32);

        /**
         * @brief 从单个 Unicode 字符构造
         *
         * @param[in] p_char Unicode 码点
         */
        explicit String(char32_t p_char);

        ~String() = default;

        String &operator=(const String &p_other) = default;
        String &operator=(String &&p_other) noexcept = default;

        // ═══════════════════════════════════════════════════════════════
        // 容量查询
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 获取字符数（不含终止符）
         *
         * @return 字符数
         */
        ARHUD_ALWAYS_INLINE int32_t Length() const
        {
            int32_t s = static_cast<int32_t>(data_.Size());
            return s > 0 ? s - 1 : 0;
        }

        /**
         * @brief 检查字符串是否为空
         *
         * @return true 为空
         */
        ARHUD_ALWAYS_INLINE bool IsEmpty() const { return Length() == 0; }

        // ═══════════════════════════════════════════════════════════════
        // 元素访问
        // ═══════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE const char32_t *Data() const
        {
            return data_.Data();
        }

        ARHUD_ALWAYS_INLINE char32_t *Data()
        {
            return data_.Data();
        }

        ARHUD_ALWAYS_INLINE char32_t operator[](int32_t p_index) const
        {
            ARHUD_ASSERT(p_index >= 0 && p_index < Length(), "String index out of bounds");
            return data_[p_index];
        }

        ARHUD_ALWAYS_INLINE char32_t &operator[](int32_t p_index)
        {
            ARHUD_ASSERT(p_index >= 0 && p_index < Length(), "String index out of bounds");
            return data_[p_index];
        }

        // ═══════════════════════════════════════════════════════════════
        // 比较运算符
        // ═══════════════════════════════════════════════════════════════

        bool operator==(const String &p_other) const;
        bool operator!=(const String &p_other) const;
        bool operator<(const String &p_other) const;
        bool operator<=(const String &p_other) const;
        bool operator>(const String &p_other) const;
        bool operator>=(const String &p_other) const;

        bool operator==(const char *p_utf8) const;
        bool operator!=(const char *p_utf8) const;

        // ═══════════════════════════════════════════════════════════════
        // 拼接运算符
        // ═══════════════════════════════════════════════════════════════

        String operator+(const String &p_other) const;
        String operator+(const char *p_utf8) const;
        String operator+(char32_t p_char) const;

        String &operator+=(const String &p_other);
        String &operator+=(const char *p_utf8);
        String &operator+=(char32_t p_char);

        // ═══════════════════════════════════════════════════════════════
        // 查找
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 查找子串（区分大小写）
         *
         * @param[in] p_str  要查找的子串
         * @param[in] p_from 起始位置（默认 0）
         *
         * @return 找到的起始位置，未找到返回 kNpos
         */
        int32_t Find(const String &p_str, int32_t p_from = 0) const;

        /**
         * @brief 查找单个字符
         *
         * @param[in] p_char 要查找的字符
         * @param[in] p_from 起始位置（默认 0）
         *
         * @return 找到的位置，未找到返回 kNpos
         */
        int32_t FindChar(char32_t p_char, int32_t p_from = 0) const;

        /**
         * @brief 从右向左查找子串
         *
         * @param[in] p_str  要查找的子串
         * @param[in] p_from 起始位置（默认 -1 表示从末尾开始）
         *
         * @return 找到的起始位置，未找到返回 kNpos
         */
        int32_t RFind(const String &p_str, int32_t p_from = -1) const;

        /**
         * @brief 从右向左查找单个字符
         *
         * @param[in] p_char 要查找的字符
         * @param[in] p_from 起始位置（默认 -1 表示从末尾开始）
         *
         * @return 找到的位置，未找到返回 kNpos
         */
        int32_t RFindChar(char32_t p_char, int32_t p_from = -1) const;

        // ═══════════════════════════════════════════════════════════════
        // 子串与修改
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 取子串
         *
         * @param[in] p_from  起始位置
         * @param[in] p_chars 字符数（-1 表示到末尾）
         *
         * @return 子串
         */
        String Substr(int32_t p_from, int32_t p_chars = -1) const;

        /**
         * @brief 替换所有匹配子串
         *
         * @param[in] p_key 被替换的子串
         * @param[in] p_with 替换为的子串
         *
         * @return 替换后的新字符串
         */
        String Replace(const String &p_key, const String &p_with) const;

        /**
         * @brief 替换所有匹配字符
         *
         * @param[in] p_key  被替换的字符
         * @param[in] p_with 替换为的字符
         *
         * @return 替换后的新字符串
         */
        String ReplaceChar(char32_t p_key, char32_t p_with) const;

        /**
         * @brief 插入子串
         *
         * @param[in] p_at_pos 插入位置
         * @param[in] p_str    要插入的字符串
         *
         * @return 插入后的新字符串
         */
        String Insert(int32_t p_at_pos, const String &p_str) const;

        /**
         * @brief 删除指定范围的字符
         *
         * @param[in] p_pos   起始位置
         * @param[in] p_chars 删除的字符数
         *
         * @return 删除后的新字符串
         */
        String Erase(int32_t p_pos, int32_t p_chars = 1) const;

        // ═══════════════════════════════════════════════════════════════
        // 前缀/后缀
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 检查是否以指定字符串开头
         *
         * @param[in] p_str 前缀字符串
         *
         * @return true 以该字符串开头
         */
        bool BeginsWith(const String &p_str) const;

        /**
         * @brief 检查是否以指定字符串结尾
         *
         * @param[in] p_str 后缀字符串
         *
         * @return true 以该字符串结尾
         */
        bool EndsWith(const String &p_str) const;

        // ═══════════════════════════════════════════════════════════════
        // 大小写转换
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 转为小写
         *
         * @return 小写字符串
         */
        String ToLower() const;

        /**
         * @brief 转为大写
         *
         * @return 大写字符串
         */
        String ToUpper() const;

        // ═══════════════════════════════════════════════════════════════
        // 去空白
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 去除首尾空白字符
         *
         * @return 去空白后的字符串
         */
        String Strip() const;

        /**
         * @brief 去除左侧空白字符
         *
         * @return 去空白后的字符串
         */
        String LStrip() const;

        /**
         * @brief 去除右侧空白字符
         *
         * @return 去空白后的字符串
         */
        String RStrip() const;

        // ═══════════════════════════════════════════════════════════════
        // 分割与拼接
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 按分隔符分割字符串
         *
         * @param[in] p_delimiter 分隔符
         * @param[in] p_allow_empty 是否保留空段（默认 true）
         *
         * @return 分割后的字符串数组
         */
        LocalVector<String> Split(const String &p_delimiter, bool p_allow_empty = true) const;

        /**
         * @brief 按字符分割字符串
         *
         * @param[in] p_delimiter 分隔字符
         * @param[in] p_allow_empty 是否保留空段（默认 true）
         *
         * @return 分割后的字符串数组
         */
        LocalVector<String> SplitChar(char32_t p_delimiter, bool p_allow_empty = true) const;

        // ═══════════════════════════════════════════════════════════════
        // 数值转换
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 转为整数
         *
         * @return 整数值
         */
        int64_t ToInt() const;

        /**
         * @brief 转为浮点数
         *
         * @return 浮点数值
         */
        double ToFloat() const;

        /**
         * @brief 检查是否为数字字符串
         *
         * @return true 是数字
         */
        bool IsNumeric() const;

        // ═══════════════════════════════════════════════════════════════
        // 编码转换
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 转为 UTF-8 编码的字节数组
         *
         * 返回的数组包含 UTF-8 字节序列 + '\0' 终止符。
         *
         * @return UTF-8 字节数组
         */
        LocalVector<char> Utf8() const;

        /**
         * @brief 获取 UTF-8 C 字符串指针
         *
         * 返回内部缓存的 UTF-8 字符串。仅在下次非 const 操作前有效。
         *
         * @return UTF-8 C 字符串指针
         */
        const char *CStr();

        /**
         * @brief 获取 UTF-8 C 字符串指针（const 版本）
         *
         * @return UTF-8 C 字符串指针
         */
        const char *CStr() const;

        // ═══════════════════════════════════════════════════════════════
        // 静态工厂方法
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 将整数转为字符串
         *
         * @param[in] p_val 整数值
         *
         * @return 字符串表示
         */
        static String Num(int64_t p_val);

        /**
         * @brief 将浮点数转为字符串
         *
         * @param[in] p_val      浮点数值
         * @param[in] p_decimals 小数位数（-1 表示自动）
         *
         * @return 字符串表示
         */
        static String NumReal(double p_val, int32_t p_decimals = -1);

        /**
         * @brief 从单个 Unicode 字符创建字符串
         *
         * @param[in] p_char Unicode 码点
         *
         * @return 包含单个字符的字符串
         */
        static String Chr(char32_t p_char);

        /**
         * @brief 格式化字符串
         *
         * 使用 {0} {1} ... 占位符，按参数顺序替换。
         *
         * @param[in] p_format 格式字符串
         * @param[in] p_args   参数数组
         * @param[in] p_count  参数数量
         *
         * @return 格式化后的字符串
         */
        static String Format(const String &p_format, const String *p_args, uint32_t p_count);

    private:
        /**
         * @brief 内部追加 UTF-8 字节序列
         *
         * @param[in] p_utf8 UTF-8 字节指针
         * @param[in] p_len  字节长度
         */
        void AppendUtf8(const char *p_utf8, int32_t p_len);

        /**
         * @brief 内部追加 UTF-32 字符序列
         *
         * @param[in] p_utf32 UTF-32 字符指针
         * @param[in] p_len   字符长度
         */
        void AppendUtf32(const char32_t *p_utf32, int32_t p_len);

        /**
         * @brief 判断字符是否为空白
         *
         * @param[in] p_c 字符
         *
         * @return true 是空白字符
         */
        static bool IsWhiteSpace(char32_t p_c);

        /**
         * @brief char32_t 转小写
         *
         * @param[in] p_c 字符
         *
         * @return 小写字符
         */
        static char32_t ToLowerChar(char32_t p_c);

        /**
         * @brief char32_t 转大写
         *
         * @param[in] p_c 字符
         *
         * @return 大写字符
         */
        static char32_t ToUpperChar(char32_t p_c);

        LocalVector<char32_t> data_;
        mutable LocalVector<char> utf8_cache_;
    };

} // namespace arhud
