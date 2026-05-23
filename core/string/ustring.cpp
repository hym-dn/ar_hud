/**
 * @file arhud_string.cpp
 * @brief UTF-32 字符串类实现
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-03
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "ustring.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // UTF-8 ↔ UTF-32 转换辅助
    // ═══════════════════════════════════════════════════════════════════════

    static int32_t DecodeUtf8Char(const char *p_utf8, char32_t &r_char)
    {
        const uint8_t *c = reinterpret_cast<const uint8_t *>(p_utf8);

        if (c[0] < 0x80)
        {
            r_char = static_cast<char32_t>(c[0]);
            return 1;
        }
        else if ((c[0] & 0xE0) == 0xC0)
        {
            r_char = (static_cast<char32_t>(c[0] & 0x1F) << 6)
                   | (static_cast<char32_t>(c[1] & 0x3F));
            return 2;
        }
        else if ((c[0] & 0xF0) == 0xE0)
        {
            r_char = (static_cast<char32_t>(c[0] & 0x0F) << 12)
                   | (static_cast<char32_t>(c[1] & 0x3F) << 6)
                   | (static_cast<char32_t>(c[2] & 0x3F));
            return 3;
        }
        else if ((c[0] & 0xF8) == 0xF0)
        {
            r_char = (static_cast<char32_t>(c[0] & 0x07) << 18)
                   | (static_cast<char32_t>(c[1] & 0x3F) << 12)
                   | (static_cast<char32_t>(c[2] & 0x3F) << 6)
                   | (static_cast<char32_t>(c[3] & 0x3F));
            return 4;
        }

        r_char = U'\0';
        return 0;
    }

    static int32_t EncodeUtf8Char(char32_t p_char, char *r_buf)
    {
        if (p_char <= 0x7F)
        {
            r_buf[0] = static_cast<char>(p_char);
            return 1;
        }
        else if (p_char <= 0x7FF)
        {
            r_buf[0] = static_cast<char>(0xC0 | (p_char >> 6));
            r_buf[1] = static_cast<char>(0x80 | (p_char & 0x3F));
            return 2;
        }
        else if (p_char <= 0xFFFF)
        {
            r_buf[0] = static_cast<char>(0xE0 | (p_char >> 12));
            r_buf[1] = static_cast<char>(0x80 | ((p_char >> 6) & 0x3F));
            r_buf[2] = static_cast<char>(0x80 | (p_char & 0x3F));
            return 3;
        }
        else
        {
            r_buf[0] = static_cast<char>(0xF0 | (p_char >> 18));
            r_buf[1] = static_cast<char>(0x80 | ((p_char >> 12) & 0x3F));
            r_buf[2] = static_cast<char>(0x80 | ((p_char >> 6) & 0x3F));
            r_buf[3] = static_cast<char>(0x80 | (p_char & 0x3F));
            return 4;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 构造函数
    // ═══════════════════════════════════════════════════════════════════════

    String::String(const char *p_utf8)
    {
        data_.PushBack(U'\0');
        if (p_utf8 != nullptr)
        {
            AppendUtf8(p_utf8, static_cast<int32_t>(std::strlen(p_utf8)));
        }
    }

    String::String(const char *p_utf8, int32_t p_len)
    {
        data_.PushBack(U'\0');
        if (p_utf8 != nullptr && p_len > 0)
        {
            AppendUtf8(p_utf8, p_len);
        }
    }

    String::String(const char32_t *p_utf32)
    {
        data_.PushBack(U'\0');
        if (p_utf32 != nullptr)
        {
            int32_t len = 0;
            while (p_utf32[len] != U'\0')
            {
                ++len;
            }
            AppendUtf32(p_utf32, len);
        }
    }

    String::String(char32_t p_char)
    {
        data_.PushBack(p_char);
        data_.PushBack(U'\0');
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 比较运算符
    // ═══════════════════════════════════════════════════════════════════════

    bool String::operator==(const String &p_other) const
    {
        if (Length() != p_other.Length())
        {
            return false;
        }
        return std::memcmp(Data(), p_other.Data(), sizeof(char32_t) * Length()) == 0;
    }

    bool String::operator!=(const String &p_other) const
    {
        return !(*this == p_other);
    }

    bool String::operator<(const String &p_other) const
    {
        int32_t min_len = Length() < p_other.Length() ? Length() : p_other.Length();
        for (int32_t i = 0; i < min_len; ++i)
        {
            if (data_[i] < p_other.data_[i])
            {
                return true;
            }
            if (data_[i] > p_other.data_[i])
            {
                return false;
            }
        }
        return Length() < p_other.Length();
    }

    bool String::operator<=(const String &p_other) const
    {
        return !(p_other < *this);
    }

    bool String::operator>(const String &p_other) const
    {
        return p_other < *this;
    }

    bool String::operator>=(const String &p_other) const
    {
        return !(*this < p_other);
    }

    bool String::operator==(const char *p_utf8) const
    {
        return *this == String(p_utf8);
    }

    bool String::operator!=(const char *p_utf8) const
    {
        return !(*this == p_utf8);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 拼接运算符
    // ═══════════════════════════════════════════════════════════════════════

    String String::operator+(const String &p_other) const
    {
        String result;
        result.data_.Clear();
        result.data_.Reserve(Length() + p_other.Length() + 1);

        for (int32_t i = 0; i < Length(); ++i)
        {
            result.data_.PushBack(data_[i]);
        }
        for (int32_t i = 0; i < p_other.Length(); ++i)
        {
            result.data_.PushBack(p_other.data_[i]);
        }
        result.data_.PushBack(U'\0');
        return result;
    }

    String String::operator+(const char *p_utf8) const
    {
        return *this + String(p_utf8);
    }

    String String::operator+(char32_t p_char) const
    {
        String result;
        result.data_.Clear();
        result.data_.Reserve(Length() + 2);

        for (int32_t i = 0; i < Length(); ++i)
        {
            result.data_.PushBack(data_[i]);
        }
        result.data_.PushBack(p_char);
        result.data_.PushBack(U'\0');
        return result;
    }

    String &String::operator+=(const String &p_other)
    {
        int32_t old_len = Length();
        data_.RemoveAt(data_.Size() - 1);

        for (int32_t i = 0; i < p_other.Length(); ++i)
        {
            data_.PushBack(p_other.data_[i]);
        }
        data_.PushBack(U'\0');
        utf8_cache_.Clear();
        return *this;
    }

    String &String::operator+=(const char *p_utf8)
    {
        if (p_utf8 != nullptr)
        {
            AppendUtf8(p_utf8, static_cast<int32_t>(std::strlen(p_utf8)));
        }
        return *this;
    }

    String &String::operator+=(char32_t p_char)
    {
        data_.RemoveAt(data_.Size() - 1);
        data_.PushBack(p_char);
        data_.PushBack(U'\0');
        utf8_cache_.Clear();
        return *this;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 查找
    // ═══════════════════════════════════════════════════════════════════════

    int32_t String::Find(const String &p_str, int32_t p_from) const
    {
        if (p_str.IsEmpty())
        {
            return 0;
        }
        if (p_from < 0)
        {
            p_from = 0;
        }
        int32_t len = Length();
        int32_t slen = p_str.Length();
        if (slen > len - p_from)
        {
            return kNpos;
        }
        for (int32_t i = p_from; i <= len - slen; ++i)
        {
            bool match = true;
            for (int32_t j = 0; j < slen; ++j)
            {
                if (data_[i + j] != p_str.data_[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return i;
            }
        }
        return kNpos;
    }

    int32_t String::FindChar(char32_t p_char, int32_t p_from) const
    {
        if (p_from < 0)
        {
            p_from = 0;
        }
        for (int32_t i = p_from; i < Length(); ++i)
        {
            if (data_[i] == p_char)
            {
                return i;
            }
        }
        return kNpos;
    }

    int32_t String::RFind(const String &p_str, int32_t p_from) const
    {
        if (p_str.IsEmpty())
        {
            return Length();
        }
        int32_t len = Length();
        int32_t slen = p_str.Length();
        if (slen > len)
        {
            return kNpos;
        }
        int32_t start = (p_from < 0 || p_from > len - slen) ? len - slen : p_from;
        for (int32_t i = start; i >= 0; --i)
        {
            bool match = true;
            for (int32_t j = 0; j < slen; ++j)
            {
                if (data_[i + j] != p_str.data_[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return i;
            }
        }
        return kNpos;
    }

    int32_t String::RFindChar(char32_t p_char, int32_t p_from) const
    {
        int32_t len = Length();
        int32_t start = (p_from < 0) ? len - 1 : p_from;
        if (start >= len)
        {
            start = len - 1;
        }
        for (int32_t i = start; i >= 0; --i)
        {
            if (data_[i] == p_char)
            {
                return i;
            }
        }
        return kNpos;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 子串与修改
    // ═══════════════════════════════════════════════════════════════════════

    String String::Substr(int32_t p_from, int32_t p_chars) const
    {
        int32_t len = Length();
        if (p_from < 0 || p_from >= len)
        {
            return String();
        }
        int32_t count = (p_chars < 0) ? (len - p_from) : p_chars;
        if (p_from + count > len)
        {
            count = len - p_from;
        }

        String result;
        result.data_.Clear();
        result.data_.Reserve(count + 1);
        for (int32_t i = 0; i < count; ++i)
        {
            result.data_.PushBack(data_[p_from + i]);
        }
        result.data_.PushBack(U'\0');
        return result;
    }

    String String::Replace(const String &p_key, const String &p_with) const
    {
        if (p_key.IsEmpty())
        {
            return *this;
        }

        String result;
        int32_t from = 0;
        while (true)
        {
            int32_t pos = Find(p_key, from);
            if (pos == kNpos)
            {
                result += Substr(from);
                break;
            }
            result += Substr(from, pos - from);
            result += p_with;
            from = pos + p_key.Length();
        }
        return result;
    }

    String String::ReplaceChar(char32_t p_key, char32_t p_with) const
    {
        String result;
        result.data_.Clear();
        result.data_.Reserve(Length() + 1);
        for (int32_t i = 0; i < Length(); ++i)
        {
            result.data_.PushBack(data_[i] == p_key ? p_with : data_[i]);
        }
        result.data_.PushBack(U'\0');
        return result;
    }

    String String::Insert(int32_t p_at_pos, const String &p_str) const
    {
        return Substr(0, p_at_pos) + p_str + Substr(p_at_pos);
    }

    String String::Erase(int32_t p_pos, int32_t p_chars) const
    {
        return Substr(0, p_pos) + Substr(p_pos + p_chars);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 前缀/后缀
    // ═══════════════════════════════════════════════════════════════════════

    bool String::BeginsWith(const String &p_str) const
    {
        if (p_str.Length() > Length())
        {
            return false;
        }
        for (int32_t i = 0; i < p_str.Length(); ++i)
        {
            if (data_[i] != p_str.data_[i])
            {
                return false;
            }
        }
        return true;
    }

    bool String::EndsWith(const String &p_str) const
    {
        if (p_str.Length() > Length())
        {
            return false;
        }
        int32_t start = Length() - p_str.Length();
        for (int32_t i = 0; i < p_str.Length(); ++i)
        {
            if (data_[start + i] != p_str.data_[i])
            {
                return false;
            }
        }
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 大小写转换
    // ═══════════════════════════════════════════════════════════════════════

    char32_t String::ToLowerChar(char32_t p_c)
    {
        if (p_c >= U'A' && p_c <= U'Z')
        {
            return p_c + (U'a' - U'A');
        }
        return p_c;
    }

    char32_t String::ToUpperChar(char32_t p_c)
    {
        if (p_c >= U'a' && p_c <= U'z')
        {
            return p_c - (U'a' - U'A');
        }
        return p_c;
    }

    String String::ToLower() const
    {
        String result;
        result.data_.Clear();
        result.data_.Reserve(Length() + 1);
        for (int32_t i = 0; i < Length(); ++i)
        {
            result.data_.PushBack(ToLowerChar(data_[i]));
        }
        result.data_.PushBack(U'\0');
        return result;
    }

    String String::ToUpper() const
    {
        String result;
        result.data_.Clear();
        result.data_.Reserve(Length() + 1);
        for (int32_t i = 0; i < Length(); ++i)
        {
            result.data_.PushBack(ToUpperChar(data_[i]));
        }
        result.data_.PushBack(U'\0');
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 去空白
    // ═══════════════════════════════════════════════════════════════════════

    bool String::IsWhiteSpace(char32_t p_c)
    {
        return p_c == U' ' || p_c == U'\t' || p_c == U'\n' || p_c == U'\r';
    }

    String String::Strip() const
    {
        return LStrip().RStrip();
    }

    String String::LStrip() const
    {
        int32_t start = 0;
        while (start < Length() && IsWhiteSpace(data_[start]))
        {
            ++start;
        }
        return Substr(start);
    }

    String String::RStrip() const
    {
        int32_t end = Length() - 1;
        while (end >= 0 && IsWhiteSpace(data_[end]))
        {
            --end;
        }
        return Substr(0, end + 1);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 分割
    // ═══════════════════════════════════════════════════════════════════════

    LocalVector<String> String::Split(const String &p_delimiter, bool p_allow_empty) const
    {
        LocalVector<String> result;
        if (p_delimiter.IsEmpty())
        {
            result.PushBack(*this);
            return result;
        }

        int32_t from = 0;
        while (true)
        {
            int32_t pos = Find(p_delimiter, from);
            if (pos == kNpos)
            {
                String part = Substr(from);
                if (p_allow_empty || !part.IsEmpty())
                {
                    result.PushBack(part);
                }
                break;
            }
            String part = Substr(from, pos - from);
            if (p_allow_empty || !part.IsEmpty())
            {
                result.PushBack(part);
            }
            from = pos + p_delimiter.Length();
        }
        return result;
    }

    LocalVector<String> String::SplitChar(char32_t p_delimiter, bool p_allow_empty) const
    {
        return Split(String(p_delimiter), p_allow_empty);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 数值转换
    // ═══════════════════════════════════════════════════════════════════════

    int64_t String::ToInt() const
    {
        LocalVector<char> utf8 = Utf8();
        return std::atoll(utf8.Data());
    }

    double String::ToFloat() const
    {
        LocalVector<char> utf8 = Utf8();
        return std::atof(utf8.Data());
    }

    bool String::IsNumeric() const
    {
        if (IsEmpty())
        {
            return false;
        }
        bool has_digit = false;
        for (int32_t i = 0; i < Length(); ++i)
        {
            char32_t c = data_[i];
            if (c >= U'0' && c <= U'9')
            {
                has_digit = true;
            }
            else if (c == U'-' && i == 0)
            {
                continue;
            }
            else if (c == U'.' || c == U'e' || c == U'E')
            {
                continue;
            }
            else
            {
                return false;
            }
        }
        return has_digit;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 编码转换
    // ═══════════════════════════════════════════════════════════════════════

    LocalVector<char> String::Utf8() const
    {
        LocalVector<char> result;
        int32_t len = Length();
        result.Reserve(len + 1);

        char buf[4];
        for (int32_t i = 0; i < len; ++i)
        {
            int32_t encoded_len = EncodeUtf8Char(data_[i], buf);
            for (int32_t j = 0; j < encoded_len; ++j)
            {
                result.PushBack(buf[j]);
            }
        }
        result.PushBack('\0');
        return result;
    }

    const char *String::CStr()
    {
        if (utf8_cache_.IsEmpty() && !IsEmpty())
        {
            utf8_cache_ = Utf8();
        }
        return utf8_cache_.IsEmpty() ? "" : utf8_cache_.Data();
    }

    const char *String::CStr() const
    {
        if (utf8_cache_.IsEmpty() && !IsEmpty())
        {
            utf8_cache_ = Utf8();
        }
        return utf8_cache_.IsEmpty() ? "" : utf8_cache_.Data();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 静态工厂方法
    // ═══════════════════════════════════════════════════════════════════════

    String String::Num(int64_t p_val)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(p_val));
        return String(buf);
    }

    String String::NumReal(double p_val, int32_t p_decimals)
    {
        char buf[64];
        if (p_decimals < 0)
        {
            std::snprintf(buf, sizeof(buf), "%.17g", p_val);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%.*f", p_decimals, p_val);
        }
        return String(buf);
    }

    String String::Chr(char32_t p_char)
    {
        return String(p_char);
    }

    String String::Format(const String &p_format, const String *p_args, uint32_t p_count)
    {
        String result;
        int32_t i = 0;
        int32_t len = p_format.Length();

        while (i < len)
        {
            if (p_format[i] == U'{' && i + 1 < len)
            {
                int32_t j = i + 1;
                while (j < len && p_format[j] != U'}')
                {
                    ++j;
                }
                if (j < len && p_format[j] == U'}')
                {
                    int32_t idx = 0;
                    for (int32_t k = i + 1; k < j; ++k)
                    {
                        char32_t c = p_format[k];
                        if (c >= U'0' && c <= U'9')
                        {
                            idx = idx * 10 + static_cast<int32_t>(c - U'0');
                        }
                    }
                    if (idx >= 0 && static_cast<uint32_t>(idx) < p_count)
                    {
                        result += p_args[idx];
                    }
                    i = j + 1;
                    continue;
                }
            }
            result += p_format[i];
            ++i;
        }
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 内部方法
    // ═══════════════════════════════════════════════════════════════════════

    void String::AppendUtf8(const char *p_utf8, int32_t p_len)
    {
        int32_t pos = 0;
        while (pos < p_len)
        {
            char32_t ch;
            int32_t consumed = DecodeUtf8Char(p_utf8 + pos, ch);
            if (consumed == 0)
            {
                ++pos;
                continue;
            }
            pos += consumed;

            data_.RemoveAt(data_.Size() - 1);
            data_.PushBack(ch);
            data_.PushBack(U'\0');
        }
        utf8_cache_.Clear();
    }

    void String::AppendUtf32(const char32_t *p_utf32, int32_t p_len)
    {
        for (int32_t i = 0; i < p_len; ++i)
        {
            data_.RemoveAt(data_.Size() - 1);
            data_.PushBack(p_utf32[i]);
            data_.PushBack(U'\0');
        }
        utf8_cache_.Clear();
    }

} // namespace arhud
