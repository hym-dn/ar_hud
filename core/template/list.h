/**
 * @file list.h
 * @brief 双向链表容器
 *
 * 参考 Godot 4.6 List 设计，去除 COW 语义和 Allocator 模板参数，
 * 统一使用 arhud::memory 全局分配器。
 *
 * 设计取舍：
 *   - 双向链表：O(1) 头尾插入/删除，O(n) 随机访问
 *   - 节点独立堆分配：每个 Element 独立 new/delete，适合频繁中间插入/删除
 *   - 无 COW：直接持有数据，无引用计数开销
 *   - Element 即迭代器：Godot 风格，Element* 同时充当链表节点和迭代器
 *   - 延迟分配 _Data：空链表不分配管理结构，节省内存
 *
 * 与 std::list 的关键区别：
 *   - 使用 arhud::memory 全局分配器（非 std::allocator）
 *   - Element* 即迭代器，可直接持有并在 O(1) 内删除
 *   - 提供 move_to_back / move_to_front / move_before 等位置操作
 *   - 不使用异常，错误通过 ARHUD_ASSERT 处理
 *
 * 典型用途：
 * - 消息队列（主线程→渲染线程命令列表）
 * - LRU 缓存（O(1) 移动到前端）
 * - 有序插入场景（中间插入频繁）
 * - 资源依赖链
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-05-06
 * @copyright Copyright (c) 2025 ARHud Project
 * @ingroup core
 */

#pragma once

#include <initializer_list>
#include <type_traits>
#include <utility>

#include "os/memory.h"
#include "typedefs.h"

namespace arhud
{

    // ═══════════════════════════════════════════════════════════════════════
    // List
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief 双向链表容器
     *
     * 节点（Element）独立堆分配，每个 Element 同时充当链表节点和迭代器。
     * 空链表不分配内部管理结构（_Data），首次插入时延迟分配。
     *
     * @tparam T 元素类型（必须可复制或可移动）
     */
    template <typename T>
    class List
    {
    public:
        // ═══════════════════════════════════════════════════════════════
        // Element（链表节点 / 迭代器）
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 链表节点，同时充当迭代器
         *
         * 持有值副本，维护前后指针和所属链表的 _Data 指针。
         * 可通过 next()/prev() 遍历，通过 get()/set() 访问值，
         * 通过 Erase() 从所属链表中移除自身。
         */
        class Element
        {
            friend class List<T>;

            T value_;
            Element *next_ptr_ = nullptr;
            Element *prev_ptr_ = nullptr;
            typename List<T>::Data *data_ = nullptr;

            Element() = default;

        public:
            /**
             * @brief 获取下一个元素
             *
             * @return 下一个元素指针，末尾返回 nullptr
             */
            ARHUD_ALWAYS_INLINE const Element *Next() const { return next_ptr_; }

            /**
             * @brief 获取下一个元素
             *
             * @return 下一个元素指针，末尾返回 nullptr
             */
            ARHUD_ALWAYS_INLINE Element *Next() { return next_ptr_; }

            /**
             * @brief 获取上一个元素
             *
             * @return 上一个元素指针，头部返回 nullptr
             */
            ARHUD_ALWAYS_INLINE const Element *Prev() const { return prev_ptr_; }

            /**
             * @brief 获取上一个元素
             *
             * @return 上一个元素指针，头部返回 nullptr
             */
            ARHUD_ALWAYS_INLINE Element *Prev() { return prev_ptr_; }

            /**
             * @brief 解引用，获取存储的值
             */
            ARHUD_ALWAYS_INLINE const T &operator*() const { return value_; }

            /**
             * @brief 解引用，获取存储的值
             */
            ARHUD_ALWAYS_INLINE T &operator*() { return value_; }

            /**
             * @brief 箭头操作符，访问值成员
             */
            ARHUD_ALWAYS_INLINE const T *operator->() const { return &value_; }

            /**
             * @brief 箭头操作符，访问值成员
             */
            ARHUD_ALWAYS_INLINE T *operator->() { return &value_; }

            /**
             * @brief 获取存储的值
             */
            ARHUD_ALWAYS_INLINE T &Get() { return value_; }

            /**
             * @brief 获取存储的值（只读）
             */
            ARHUD_ALWAYS_INLINE const T &Get() const { return value_; }

            /**
             * @brief 设置存储的值
             *
             * @param[in] p_value 新值
             */
            ARHUD_ALWAYS_INLINE void Set(const T &p_value) { value_ = p_value; }

            /**
             * @brief 从所属链表中移除自身
             */
            void Erase() { data_->list_->Erase(this); }
        };

        // ═══════════════════════════════════════════════════════════════
        // STL 风格迭代器
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 只读正向迭代器
         */
        class ConstIterator
        {
        public:
            ARHUD_ALWAYS_INLINE const T &operator*() const { return elem_->Get(); }
            ARHUD_ALWAYS_INLINE const T *operator->() const { return &elem_->Get(); }

            ARHUD_ALWAYS_INLINE ConstIterator &operator++()
            {
                elem_ = elem_->Next();
                return *this;
            }

            ARHUD_ALWAYS_INLINE ConstIterator &operator--()
            {
                elem_ = elem_->Prev();
                return *this;
            }

            ARHUD_ALWAYS_INLINE bool operator==(const ConstIterator &p_other) const
            {
                return elem_ == p_other.elem_;
            }

            ARHUD_ALWAYS_INLINE bool operator!=(const ConstIterator &p_other) const
            {
                return elem_ != p_other.elem_;
            }

            ConstIterator() = default;
            explicit ConstIterator(const Element *p_elem) : elem_(p_elem) {}
            ConstIterator(const ConstIterator &p_other) = default;

        private:
            const Element *elem_ = nullptr;
        };

        /**
         * @brief 正向迭代器
         */
        class Iterator
        {
        public:
            ARHUD_ALWAYS_INLINE T &operator*() const { return elem_->Get(); }
            ARHUD_ALWAYS_INLINE T *operator->() const { return &elem_->Get(); }

            ARHUD_ALWAYS_INLINE Iterator &operator++()
            {
                elem_ = elem_->Next();
                return *this;
            }

            ARHUD_ALWAYS_INLINE Iterator &operator--()
            {
                elem_ = elem_->Prev();
                return *this;
            }

            ARHUD_ALWAYS_INLINE bool operator==(const Iterator &p_other) const
            {
                return elem_ == p_other.elem_;
            }

            ARHUD_ALWAYS_INLINE bool operator!=(const Iterator &p_other) const
            {
                return elem_ != p_other.elem_;
            }

            Iterator() = default;
            explicit Iterator(Element *p_elem) : elem_(p_elem) {}
            Iterator(const Iterator &p_other) = default;

            /**
             * @brief 隐式转换为 ConstIterator
             */
            operator ConstIterator() const { return ConstIterator(elem_); }

        private:
            Element *elem_ = nullptr;
        };

        // ═══════════════════════════════════════════════════════════════
        // 类型别名
        // ═══════════════════════════════════════════════════════════════

        using ValueType = T;

        // ═══════════════════════════════════════════════════════════════
        // 构造 / 析构 / 赋值
        // ═══════════════════════════════════════════════════════════════

        List() = default;

        List(std::initializer_list<T> p_init)
        {
            for (const T &val : p_init)
            {
                PushBack(val);
            }
        }

        List(const List &p_other)
        {
            const Element *it = p_other.Front();
            while (it != nullptr)
            {
                PushBack(it->Get());
                it = it->Next();
            }
        }

        List(List &&p_other) noexcept
            : data_(p_other.data_)
        {
            p_other.data_ = nullptr;
        }

        ~List()
        {
            Clear();
            if (data_ != nullptr)
            {
                ARHUD_ASSERT(data_->size_cache == 0, "List destructor: size must be 0 after Clear");
                ARHUD_DELETE(data_);
            }
        }

        List &operator=(const List &p_other)
        {
            if (this != &p_other)
            {
                Clear();
                const Element *it = p_other.Front();
                while (it != nullptr)
                {
                    PushBack(it->Get());
                    it = it->Next();
                }
            }
            return *this;
        }

        List &operator=(List &&p_other) noexcept
        {
            if (this != &p_other)
            {
                Clear();
                if (data_ != nullptr)
                {
                    ARHUD_DELETE(data_);
                    data_ = nullptr;
                }
                data_ = p_other.data_;
                p_other.data_ = nullptr;
            }
            return *this;
        }

        // ═══════════════════════════════════════════════════════════════
        // 元素访问
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 获取首元素节点
         *
         * @return 首元素指针，空链表返回 nullptr
         */
        ARHUD_ALWAYS_INLINE const Element *Front() const
        {
            return data_ != nullptr ? data_->first : nullptr;
        }

        /**
         * @brief 获取首元素节点
         *
         * @return 首元素指针，空链表返回 nullptr
         */
        ARHUD_ALWAYS_INLINE Element *Front()
        {
            return data_ != nullptr ? data_->first : nullptr;
        }

        /**
         * @brief 获取尾元素节点
         *
         * @return 尾元素指针，空链表返回 nullptr
         */
        ARHUD_ALWAYS_INLINE const Element *Back() const
        {
            return data_ != nullptr ? data_->last : nullptr;
        }

        /**
         * @brief 获取尾元素节点
         *
         * @return 尾元素指针，空链表返回 nullptr
         */
        ARHUD_ALWAYS_INLINE Element *Back()
        {
            return data_ != nullptr ? data_->last : nullptr;
        }

        /**
         * @brief 按索引随机访问（O(n)，慎用）
         *
         * @param[in] p_index 索引（从 0 开始）
         *
         * @return 元素值的引用
         *
         * @pre p_index < Size()
         */
        T &Get(uint32_t p_index)
        {
            ARHUD_ASSERT(data_ != nullptr && p_index < static_cast<uint32_t>(data_->size_cache),
                         "List::Get() index out of bounds");
            Element *it = data_->first;
            for (uint32_t i = 0; i < p_index; ++i)
            {
                it = it->next_ptr_;
            }
            return it->Get();
        }

        /**
         * @brief 按索引随机访问（O(n)，慎用，只读）
         *
         * @param[in] p_index 索引（从 0 开始）
         *
         * @return 元素值的只读引用
         *
         * @pre p_index < Size()
         */
        const T &Get(uint32_t p_index) const
        {
            ARHUD_ASSERT(data_ != nullptr && p_index < static_cast<uint32_t>(data_->size_cache),
                         "List::Get() index out of bounds");
            const Element *it = data_->first;
            for (uint32_t i = 0; i < p_index; ++i)
            {
                it = it->Next();
            }
            return it->Get();
        }

        // ═══════════════════════════════════════════════════════════════
        // 容量查询
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 获取元素数量
         */
        ARHUD_ALWAYS_INLINE uint32_t Size() const
        {
            return data_ != nullptr ? static_cast<uint32_t>(data_->size_cache) : 0;
        }

        /**
         * @brief 检查链表是否为空
         */
        ARHUD_ALWAYS_INLINE bool IsEmpty() const
        {
            return data_ == nullptr || data_->size_cache == 0;
        }

        // ═══════════════════════════════════════════════════════════════
        // 迭代器
        // ═══════════════════════════════════════════════════════════════

        ARHUD_ALWAYS_INLINE Iterator Begin() { return Iterator(Front()); }
        ARHUD_ALWAYS_INLINE Iterator End() { return Iterator(nullptr); }
        ARHUD_ALWAYS_INLINE ConstIterator Begin() const { return ConstIterator(Front()); }
        ARHUD_ALWAYS_INLINE ConstIterator End() const { return ConstIterator(nullptr); }

        // ═══════════════════════════════════════════════════════════════
        // 插入操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 尾部插入元素
         *
         * @param[in] p_value 要插入的值
         *
         * @return 新元素的指针
         */
        Element *PushBack(const T &p_value)
        {
            EnsureData();

            Element *n = ARHUD_NEW(Element);
            n->value_ = p_value;
            n->prev_ptr_ = data_->last;
            n->next_ptr_ = nullptr;
            n->data_ = data_;

            if (data_->last != nullptr)
            {
                data_->last->next_ptr_ = n;
            }
            data_->last = n;

            if (data_->first == nullptr)
            {
                data_->first = n;
            }

            data_->size_cache++;
            return n;
        }

        /**
         * @brief 尾部插入元素（移动语义）
         *
         * @param[in] p_value 要插入的值（右值）
         *
         * @return 新元素的指针
         */
        Element *PushBack(T &&p_value)
        {
            EnsureData();

            Element *n = ARHUD_NEW(Element);
            n->value_ = std::move(p_value);
            n->prev_ptr_ = data_->last;
            n->next_ptr_ = nullptr;
            n->data_ = data_;

            if (data_->last != nullptr)
            {
                data_->last->next_ptr_ = n;
            }
            data_->last = n;

            if (data_->first == nullptr)
            {
                data_->first = n;
            }

            data_->size_cache++;
            return n;
        }

        /**
         * @brief 头部插入元素
         *
         * @param[in] p_value 要插入的值
         *
         * @return 新元素的指针
         */
        Element *PushFront(const T &p_value)
        {
            EnsureData();

            Element *n = ARHUD_NEW(Element);
            n->value_ = p_value;
            n->prev_ptr_ = nullptr;
            n->next_ptr_ = data_->first;
            n->data_ = data_;

            if (data_->first != nullptr)
            {
                data_->first->prev_ptr_ = n;
            }
            data_->first = n;

            if (data_->last == nullptr)
            {
                data_->last = n;
            }

            data_->size_cache++;
            return n;
        }

        /**
         * @brief 头部插入元素（移动语义）
         *
         * @param[in] p_value 要插入的值（右值）
         *
         * @return 新元素的指针
         */
        Element *PushFront(T &&p_value)
        {
            EnsureData();

            Element *n = ARHUD_NEW(Element);
            n->value_ = std::move(p_value);
            n->prev_ptr_ = nullptr;
            n->next_ptr_ = data_->first;
            n->data_ = data_;

            if (data_->first != nullptr)
            {
                data_->first->prev_ptr_ = n;
            }
            data_->first = n;

            if (data_->last == nullptr)
            {
                data_->last = n;
            }

            data_->size_cache++;
            return n;
        }

        /**
         * @brief 在指定元素之后插入
         *
         * 若 p_element 为 nullptr，等同于 PushBack。
         *
         * @param[in] p_element 目标元素（必须属于本链表）
         * @param[in] p_value   要插入的值
         *
         * @return 新元素的指针
         */
        Element *InsertAfter(Element *p_element, const T &p_value)
        {
            ARHUD_ASSERT(p_element == nullptr || (data_ != nullptr && p_element->data_ == data_),
                         "List::InsertAfter() element does not belong to this list");

            if (p_element == nullptr)
            {
                return PushBack(p_value);
            }

            Element *n = ARHUD_NEW(Element);
            n->value_ = p_value;
            n->prev_ptr_ = p_element;
            n->next_ptr_ = p_element->next_ptr_;
            n->data_ = data_;

            if (p_element->next_ptr_ == nullptr)
            {
                data_->last = n;
            }
            else
            {
                p_element->next_ptr_->prev_ptr_ = n;
            }

            p_element->next_ptr_ = n;
            data_->size_cache++;
            return n;
        }

        /**
         * @brief 在指定元素之前插入
         *
         * 若 p_element 为 nullptr，等同于 PushBack。
         *
         * @param[in] p_element 目标元素（必须属于本链表）
         * @param[in] p_value   要插入的值
         *
         * @return 新元素的指针
         */
        Element *InsertBefore(Element *p_element, const T &p_value)
        {
            ARHUD_ASSERT(p_element == nullptr || (data_ != nullptr && p_element->data_ == data_),
                         "List::InsertBefore() element does not belong to this list");

            if (p_element == nullptr)
            {
                return PushBack(p_value);
            }

            Element *n = ARHUD_NEW(Element);
            n->value_ = p_value;
            n->prev_ptr_ = p_element->prev_ptr_;
            n->next_ptr_ = p_element;
            n->data_ = data_;

            if (p_element->prev_ptr_ == nullptr)
            {
                data_->first = n;
            }
            else
            {
                p_element->prev_ptr_->next_ptr_ = n;
            }

            p_element->prev_ptr_ = n;
            data_->size_cache++;
            return n;
        }

        // ═══════════════════════════════════════════════════════════════
        // 删除操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 移除尾部元素
         */
        void PopBack()
        {
            if (data_ != nullptr && data_->last != nullptr)
            {
                Erase(data_->last);
            }
        }

        /**
         * @brief 移除头部元素
         */
        void PopFront()
        {
            if (data_ != nullptr && data_->first != nullptr)
            {
                Erase(data_->first);
            }
        }

        /**
         * @brief 按元素指针移除
         *
         * @param[in] p_element 要移除的元素（必须属于本链表）
         *
         * @return true 成功移除
         * @retval false 元素为空或不属于本链表
         */
        bool Erase(Element *p_element)
        {
            if (data_ == nullptr || p_element == nullptr)
            {
                return false;
            }

            ARHUD_ASSERT(p_element->data_ == data_,
                         "List::Erase() element does not belong to this list");

            if (data_->first == p_element)
            {
                data_->first = p_element->next_ptr_;
            }

            if (data_->last == p_element)
            {
                data_->last = p_element->prev_ptr_;
            }

            if (p_element->prev_ptr_ != nullptr)
            {
                p_element->prev_ptr_->next_ptr_ = p_element->next_ptr_;
            }

            if (p_element->next_ptr_ != nullptr)
            {
                p_element->next_ptr_->prev_ptr_ = p_element->prev_ptr_;
            }

            ARHUD_DELETE(p_element);
            data_->size_cache--;

            if (data_->size_cache == 0)
            {
                ARHUD_DELETE(data_);
                data_ = nullptr;
            }

            return true;
        }

        /**
         * @brief 按值查找并移除第一个匹配元素
         *
         * @param[in] p_value 要移除的值
         *
         * @return true 成功移除
         * @retval false 未找到
         */
        bool Erase(const T &p_value)
        {
            Element *elem = Find(p_value);
            return Erase(elem);
        }

        /**
         * @brief 清空所有元素
         */
        void Clear()
        {
            while (Front() != nullptr)
            {
                Erase(Front());
            }
        }

        // ═══════════════════════════════════════════════════════════════
        // 查找
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 按值查找元素
         *
         * @param[in] p_value 要查找的值
         *
         * @return 匹配元素的指针，未找到返回 nullptr
         */
        Element *Find(const T &p_value)
        {
            Element *it = Front();
            while (it != nullptr)
            {
                if (it->value_ == p_value)
                {
                    return it;
                }
                it = it->next_ptr_;
            }
            return nullptr;
        }

        /**
         * @brief 按值查找元素（只读）
         *
         * @param[in] p_value 要查找的值
         *
         * @return 匹配元素的只读指针，未找到返回 nullptr
         */
        const Element *Find(const T &p_value) const
        {
            const Element *it = Front();
            while (it != nullptr)
            {
                if (it->value_ == p_value)
                {
                    return it;
                }
                it = it->Next();
            }
            return nullptr;
        }

        // ═══════════════════════════════════════════════════════════════
        // 位置操作
        // ═══════════════════════════════════════════════════════════════

        /**
         * @brief 将元素移动到链表尾部
         *
         * @param[in] p_element 要移动的元素（必须属于本链表）
         */
        void MoveToBack(Element *p_element)
        {
            ARHUD_ASSERT(data_ != nullptr && p_element->data_ == data_,
                         "List::MoveToBack() element does not belong to this list");

            if (p_element->next_ptr_ == nullptr)
            {
                return;
            }

            if (data_->first == p_element)
            {
                data_->first = p_element->next_ptr_;
            }

            if (data_->last == p_element)
            {
                data_->last = p_element->prev_ptr_;
            }

            if (p_element->prev_ptr_ != nullptr)
            {
                p_element->prev_ptr_->next_ptr_ = p_element->next_ptr_;
            }

            p_element->next_ptr_->prev_ptr_ = p_element->prev_ptr_;

            data_->last->next_ptr_ = p_element;
            p_element->prev_ptr_ = data_->last;
            p_element->next_ptr_ = nullptr;
            data_->last = p_element;
        }

        /**
         * @brief 将元素移动到链表头部
         *
         * @param[in] p_element 要移动的元素（必须属于本链表）
         */
        void MoveToFront(Element *p_element)
        {
            ARHUD_ASSERT(data_ != nullptr && p_element->data_ == data_,
                         "List::MoveToFront() element does not belong to this list");

            if (p_element->prev_ptr_ == nullptr)
            {
                return;
            }

            if (data_->first == p_element)
            {
                data_->first = p_element->next_ptr_;
            }

            if (data_->last == p_element)
            {
                data_->last = p_element->prev_ptr_;
            }

            p_element->prev_ptr_->next_ptr_ = p_element->next_ptr_;

            if (p_element->next_ptr_ != nullptr)
            {
                p_element->next_ptr_->prev_ptr_ = p_element->prev_ptr_;
            }

            data_->first->prev_ptr_ = p_element;
            p_element->next_ptr_ = data_->first;
            p_element->prev_ptr_ = nullptr;
            data_->first = p_element;
        }

        /**
         * @brief 将元素移动到目标元素之前
         *
         * @param[in] p_element 要移动的元素
         * @param[in] p_where   目标位置（nullptr 表示尾部）
         */
        void MoveBefore(Element *p_element, Element *p_where)
        {
            ARHUD_ASSERT(data_ != nullptr && p_element->data_ == data_,
                         "List::MoveBefore() element does not belong to this list");

            if (p_element->prev_ptr_ != nullptr)
            {
                p_element->prev_ptr_->next_ptr_ = p_element->next_ptr_;
            }
            else
            {
                data_->first = p_element->next_ptr_;
            }

            if (p_element->next_ptr_ != nullptr)
            {
                p_element->next_ptr_->prev_ptr_ = p_element->prev_ptr_;
            }
            else
            {
                data_->last = p_element->prev_ptr_;
            }

            p_element->next_ptr_ = p_where;

            if (p_where == nullptr)
            {
                p_element->prev_ptr_ = data_->last;
                data_->last = p_element;
                return;
            }

            p_element->prev_ptr_ = p_where->prev_ptr_;

            if (p_where->prev_ptr_ != nullptr)
            {
                p_where->prev_ptr_->next_ptr_ = p_element;
            }
            else
            {
                data_->first = p_element;
            }

            p_where->prev_ptr_ = p_element;
        }

        /**
         * @brief 交换两个元素的位置
         *
         * @param[in] p_a 元素 A（必须属于本链表）
         * @param[in] p_b 元素 B（必须属于本链表）
         */
        void Swap(Element *p_a, Element *p_b)
        {
            ARHUD_ASSERT(p_a != nullptr && p_b != nullptr, "List::Swap() null element");
            ARHUD_ASSERT(data_ != nullptr && p_a->data_ == data_ && p_b->data_ == data_,
                         "List::Swap() element does not belong to this list");

            if (p_a == p_b)
            {
                return;
            }

            Element *a_prev = p_a->prev_ptr_;
            Element *a_next = p_a->next_ptr_;
            Element *b_prev = p_b->prev_ptr_;
            Element *b_next = p_b->next_ptr_;

            if (a_prev != nullptr)
            {
                a_prev->next_ptr_ = p_b;
            }
            else
            {
                data_->first = p_b;
            }

            if (b_prev != nullptr)
            {
                b_prev->next_ptr_ = p_a;
            }
            else
            {
                data_->first = p_a;
            }

            if (a_next != nullptr)
            {
                a_next->prev_ptr_ = p_b;
            }
            else
            {
                data_->last = p_b;
            }

            if (b_next != nullptr)
            {
                b_next->prev_ptr_ = p_a;
            }
            else
            {
                data_->last = p_a;
            }

            p_a->prev_ptr_ = (a_next == p_b) ? p_b : b_prev;
            p_a->next_ptr_ = (b_next == p_a) ? p_b : b_next;
            p_b->prev_ptr_ = (b_next == p_a) ? p_a : a_prev;
            p_b->next_ptr_ = (a_next == p_b) ? p_a : a_next;
        }

        /**
         * @brief 反转链表
         */
        void Reverse()
        {
            uint32_t s = Size() / 2;
            Element *f = Front();
            Element *b = Back();
            for (uint32_t i = 0; i < s; ++i)
            {
                T tmp = std::move(f->value_);
                f->value_ = std::move(b->value_);
                b->value_ = std::move(tmp);
                f = f->Next();
                b = b->Prev();
            }
        }

        /**
         * @brief 将元素转移到另一个链表的尾部
         *
         * @param[in] p_element  要转移的元素（必须属于本链表）
         * @param[in] p_dst_list 目标链表
         */
        void TransferToBack(Element *p_element, List<T> &p_dst_list)
        {
            ARHUD_ASSERT(data_ != nullptr && p_element->data_ == data_,
                         "List::TransferToBack() element does not belong to this list");

            if (data_->first == p_element)
            {
                data_->first = p_element->next_ptr_;
            }
            if (data_->last == p_element)
            {
                data_->last = p_element->prev_ptr_;
            }
            if (p_element->prev_ptr_ != nullptr)
            {
                p_element->prev_ptr_->next_ptr_ = p_element->next_ptr_;
            }
            if (p_element->next_ptr_ != nullptr)
            {
                p_element->next_ptr_->prev_ptr_ = p_element->prev_ptr_;
            }
            data_->size_cache--;

            if (data_->size_cache == 0)
            {
                ARHUD_DELETE(data_);
                data_ = nullptr;
            }

            p_dst_list.EnsureData();

            if (p_dst_list.data_->last != nullptr)
            {
                p_dst_list.data_->last->next_ptr_ = p_element;
                p_element->prev_ptr_ = p_dst_list.data_->last;
            }
            else
            {
                p_dst_list.data_->first = p_element;
                p_element->prev_ptr_ = nullptr;
            }

            p_dst_list.data_->last = p_element;
            p_element->next_ptr_ = nullptr;
            p_element->data_ = p_dst_list.data_;
            p_dst_list.data_->size_cache++;
        }

    private:
        /**
         * @brief 内部管理结构（延迟分配）
         *
         * 持有首尾指针和元素计数。
         * 空链表不分配此结构，首次插入时通过 EnsureData() 创建。
         */
        struct Data
        {
            List *list_ = nullptr;
            Element *first = nullptr;
            Element *last = nullptr;
            int32_t size_cache = 0;
        };

        Data *data_ = nullptr;

        /**
         * @brief 确保 data_ 已分配
         *
         * 首次插入元素时调用，延迟分配管理结构。
         */
        void EnsureData()
        {
            if (data_ == nullptr)
            {
                data_ = ARHUD_NEW(Data);
                data_->list_ = this;
            }
        }
    };

} // namespace arhud
