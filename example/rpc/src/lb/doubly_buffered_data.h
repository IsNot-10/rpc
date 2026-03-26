// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef BUTIL_DOUBLY_BUFFERED_DATA_H
#define BUTIL_DOUBLY_BUFFERED_DATA_H

#include <vector>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <cassert>
#include <type_traits>

namespace butil {

// 辅助宏，禁止复制和赋值操作
#define BUTIL_DISALLOW_COPY_AND_ASSIGN(TypeName) \
    TypeName(const TypeName&) = delete; \
    void operator=(const TypeName&) = delete

/**
 * @brief 删除对象的函数模板
 * 
 * @tparam T 对象类型
 * @param arg 需要删除的对象指针
 */
template <typename T>
inline void delete_object(void* arg) {
    delete static_cast<T*>(arg);
}

/**
 * @brief 空类型，用于无 TLS 的情况
 */
class Void { };

/**
 * @brief 双缓冲数据结构
 * 
 * 实现了无锁读、加锁写的双缓冲机制，支持高并发场景
 * 
 * 设计特点：
 * 1. 两个数据副本，一个用于读，一个用于写
 * 2. 读操作无锁，通过原子索引切换数据副本
 * 3. 写操作加锁，修改备份副本后切换索引
 * 4. 支持线程本地存储 (TLS)
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型，默认为 Void
 */
template <typename T, typename TLS = Void>
class DoublyBufferedData {
    class Wrapper; 
public:
    /**
     * @brief 双缓冲数据的智能指针类
     * 
     * 用于安全地读取双缓冲数据，自动管理读取锁的生命周期
     */
    class ScopedPtr {
    friend class DoublyBufferedData;
    public:
        /**
         * @brief 构造函数
         */
        ScopedPtr() : _data(NULL), _w(NULL) {}
        
        /**
         * @brief 析构函数
         */
        ~ScopedPtr() {
            if (_w) {
                _w->EndRead(); 
            }
        }
        
        /**
         * @brief 获取数据指针
         * 
         * @return const T* 数据指针
         */
        const T* get() const { return _data; }
        
        /**
         * @brief 解引用操作符
         * 
         * @return const T& 数据引用
         */
        const T& operator*() const { return *_data; }
        
        /**
         * @brief 成员访问操作符
         * 
         * @return const T* 数据指针
         */
        const T* operator->() const { return _data; }
        
        /**
         * @brief 获取线程本地存储
         * 
         * @return TLS& 线程本地存储引用
         */
        TLS& tls() { return _w->user_tls(); }
        
    private:
        BUTIL_DISALLOW_COPY_AND_ASSIGN(ScopedPtr); 
        const T* _data; 
        Wrapper* _w; 
    };
    
    /**
     * @brief 构造函数
     */
    DoublyBufferedData();
    
    /**
     * @brief 析构函数
     */
    ~DoublyBufferedData();

    /**
     * @brief 读取数据
     * 
     * @param ptr 输出参数，用于存储读取到的数据
     * @return int 成功返回 0，失败返回 -1
     */
    int Read(ScopedPtr* ptr);

    /**
     * @brief 修改数据
     * 
     * @tparam Fn 函数类型，签名为 size_t(T&)
     * @param fn 修改数据的函数
     * @return size_t 函数返回值
     */
    template <typename Fn>
    size_t Modify(Fn fn);
    
    /**
     * @brief 修改数据（带一个参数）
     * 
     * @tparam Fn 函数类型，签名为 size_t(T&, const Arg1&)
     * @tparam Arg1 参数类型
     * @param fn 修改数据的函数
     * @param arg1 函数参数
     * @return size_t 函数返回值
     */
    template <typename Fn, typename Arg1>
    size_t Modify(Fn fn, const Arg1&);
    
    /**
     * @brief 修改数据（带两个参数）
     * 
     * @tparam Fn 函数类型，签名为 size_t(T&, const Arg1&, const Arg2&)
     * @tparam Arg1 第一个参数类型
     * @tparam Arg2 第二个参数类型
     * @param fn 修改数据的函数
     * @param arg1 第一个函数参数
     * @param arg2 第二个函数参数
     * @return size_t 函数返回值
     */
    template <typename Fn, typename Arg1, typename Arg2>
    size_t Modify(Fn fn, const Arg1&, const Arg2&);

    /**
     * @brief 使用前台数据修改数据
     * 
     * @tparam Fn 函数类型，签名为 size_t(T&, const T&)
     * @param fn 修改数据的函数，第一个参数是后台数据，第二个参数是前台数据
     * @return size_t 函数返回值
     */
    template <typename Fn>
    size_t ModifyWithForeground(Fn fn);
    
    /**
     * @brief 使用前台数据修改数据（带一个参数）
     * 
     * @tparam Fn 函数类型，签名为 size_t(T&, const T&, const Arg1&)
     * @tparam Arg1 参数类型
     * @param fn 修改数据的函数
     * @param arg1 函数参数
     * @return size_t 函数返回值
     */
    template <typename Fn, typename Arg1>
    size_t ModifyWithForeground(Fn fn, const Arg1&);
    
    /**
     * @brief 使用前台数据修改数据（带两个参数）
     * 
     * @tparam Fn 函数类型，签名为 size_t(T&, const T&, const Arg1&, const Arg2&)
     * @tparam Arg1 第一个参数类型
     * @tparam Arg2 第二个参数类型
     * @param fn 修改数据的函数
     * @param arg1 第一个函数参数
     * @param arg2 第二个函数参数
     * @return size_t 函数返回值
     */
    template <typename Fn, typename Arg1, typename Arg2>
    size_t ModifyWithForeground(Fn fn, const Arg1&, const Arg2&);
    
private:
    /**
     * @brief 初始化数据（针对基本类型）
     * 
     * @tparam T2 数据类型
     */
    template <typename T2>
    typename std::enable_if<std::is_integral<T2>::value || std::is_floating_point<T2>::value ||
                            std::is_pointer<T2>::value || std::is_member_function_pointer<T2>::value, void>::type
    InitializeData() {
        _data[0] = T2(); 
        _data[1] = T2(); 
    }

    /**
     * @brief 初始化数据（针对非基本类型）
     * 
     * @tparam T2 数据类型
     */
    template <typename T2>
    typename std::enable_if<!(std::is_integral<T2>::value || std::is_floating_point<T2>::value ||
                              std::is_pointer<T2>::value || std::is_member_function_pointer<T2>::value), void>::type
    InitializeData() {}

    /**
     * @brief 带前台数据的闭包（无参数）
     */
    template <typename Fn>
    struct WithFG0 {
        WithFG0(Fn& fn, T* data) : _fn(fn), _data(data) { }
        size_t operator()(T& bg) {
            return _fn(bg, (const T&)_data[&bg == _data]); 
        }
    private:
        Fn& _fn; 
        T* _data; 
    };

    /**
     * @brief 带前台数据的闭包（一个参数）
     */
    template <typename Fn, typename Arg1>
    struct WithFG1 {
        WithFG1(Fn& fn, T* data, const Arg1& arg1)
            : _fn(fn), _data(data), _arg1(arg1) {}
        size_t operator()(T& bg) {
            return _fn(bg, (const T&)_data[&bg == _data], _arg1); 
        }
    private:
        Fn& _fn; 
        T* _data; 
        const Arg1& _arg1; 
    };

    /**
     * @brief 带前台数据的闭包（两个参数）
     */
    template <typename Fn, typename Arg1, typename Arg2>
    struct WithFG2 {
        WithFG2(Fn& fn, T* data, const Arg1& arg1, const Arg2& arg2)
            : _fn(fn), _data(data), _arg1(arg1), _arg2(arg2) {}
        size_t operator()(T& bg) {
            return _fn(bg, (const T&)_data[&bg == _data], _arg1, _arg2); 
        }
    private:
        Fn& _fn; 
        T* _data; 
        const Arg1& _arg1; 
        const Arg2& _arg2; 
    };

    /**
     * @brief 单参数闭包
     */
    template <typename Fn, typename Arg1>
    struct Closure1 {
        Closure1(Fn& fn, const Arg1& arg1) : _fn(fn), _arg1(arg1) {}
        size_t operator()(T& bg) { return _fn(bg, _arg1); } 
    private:
        Fn& _fn; 
        const Arg1& _arg1; 
    };

    /**
     * @brief 双参数闭包
     */
    template <typename Fn, typename Arg1, typename Arg2>
    struct Closure2 {
        Closure2(Fn& fn, const Arg1& arg1, const Arg2& arg2)
            : _fn(fn), _arg1(arg1), _arg2(arg2) {}
        size_t operator()(T& bg) { return _fn(bg, _arg1, _arg2); } 
    private:
        Fn& _fn; 
        const Arg1& _arg1; 
        const Arg2& _arg2; 
    };

    /**
     * @brief 不安全地读取数据（无锁）
     * 
     * @return const T* 数据指针
     */
    const T* UnsafeRead() const {
        return _data + _index.load(std::memory_order_acquire); 
    }
    
    /**
     * @brief 添加 Wrapper
     * 
     * @return Wrapper* Wrapper 指针
     */
    Wrapper* AddWrapper();
    
    /**
     * @brief 移除 Wrapper
     * 
     * @param w Wrapper 指针
     */
    void RemoveWrapper(Wrapper*);

    T _data[2]; 
    std::atomic<int> _index; 
    bool _created_key; 
    pthread_key_t _wrapper_key; 
    std::vector<Wrapper*> _wrappers; 
    std::mutex _wrappers_mutex; 
    std::mutex _modify_mutex; 
};

/**
 * @brief 双缓冲数据 Wrapper 基类（带 TLS）
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 */
template <typename T, typename TLS>
class DoublyBufferedDataWrapperBase {
public:
    /**
     * @brief 获取线程本地存储
     * 
     * @return TLS& 线程本地存储引用
     */
    TLS& user_tls() { return _user_tls; }
protected:
    TLS _user_tls; 
};

/**
 * @brief 双缓冲数据 Wrapper 基类（无 TLS）
 * 
 * @tparam T 数据类型
 */
template <typename T>
class DoublyBufferedDataWrapperBase<T, Void> {
};

/**
 * @brief 双缓冲数据的 Wrapper 类
 * 
 * 用于管理每个线程的读取锁
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 */
template <typename T, typename TLS>
class DoublyBufferedData<T, TLS>::Wrapper
    : public DoublyBufferedDataWrapperBase<T, TLS> {
friend class DoublyBufferedData;
public:
    /**
     * @brief 构造函数
     * 
     * @param c DoublyBufferedData 指针
     */
    explicit Wrapper(DoublyBufferedData* c) : _control(c) {
    }
    
    /**
     * @brief 析构函数
     */
    ~Wrapper() {
        if (_control != NULL) {
            _control->RemoveWrapper(this); 
        }
    }

    /**
     * @brief 开始读取
     */
    inline void BeginRead() {
        _mutex.lock(); 
    }

    /**
     * @brief 结束读取
     */
    inline void EndRead() {
        _mutex.unlock(); 
    }

    /**
     * @brief 等待读取完成
     */
    inline void WaitReadDone() {
        std::lock_guard<std::mutex> lock(_mutex); 
    }
    
private:
    DoublyBufferedData* _control; 
    std::mutex _mutex; 
};

/**
 * @brief 添加 Wrapper
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @return Wrapper* Wrapper 指针
 */
template <typename T, typename TLS>
typename DoublyBufferedData<T, TLS>::Wrapper*
DoublyBufferedData<T, TLS>::AddWrapper() {
    std::unique_ptr<Wrapper> w(new (std::nothrow) Wrapper(this)); 
    if (NULL == w) {
        return NULL; 
    }
    {
        std::lock_guard<std::mutex> lock(_wrappers_mutex); 
        _wrappers.push_back(w.get()); 
    }
    return w.release();
}

/**
 * @brief 移除 Wrapper
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @param w Wrapper 指针
 */
template <typename T, typename TLS>
void DoublyBufferedData<T, TLS>::RemoveWrapper(
    typename DoublyBufferedData<T, TLS>::Wrapper* w) {
    if (NULL == w) {
        return; 
    }
    std::lock_guard<std::mutex> lock(_wrappers_mutex); 
    for (size_t i = 0; i < _wrappers.size(); ++i) {
        if (_wrappers[i] == w) { 
            _wrappers[i] = _wrappers.back(); 
            _wrappers.pop_back(); 
            return;
        }
    }
}

/**
 * @brief 构造函数
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 */
template <typename T, typename TLS>
DoublyBufferedData<T, TLS>::DoublyBufferedData()
    : _index(0) 
    , _created_key(false) 
    , _wrapper_key(0) 
{
    _wrappers.reserve(64); 
    const int rc = pthread_key_create(&_wrapper_key, 
                                      butil::delete_object<Wrapper>); 
    if (rc != 0) {
        std::cerr << "Fail to pthread_key_create: " << strerror(rc) << std::endl; 
        exit(1); 
    } else {
        _created_key = true; 
    }
    
    InitializeData<T>(); 
}

/**
 * @brief 析构函数
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 */
template <typename T, typename TLS>
DoublyBufferedData<T, TLS>::~DoublyBufferedData() {
    if (_created_key) {
        pthread_key_delete(_wrapper_key); 
    }
    
    {
        std::lock_guard<std::mutex> lock(_wrappers_mutex); 
        for (size_t i = 0; i < _wrappers.size(); ++i) {
            _wrappers[i]->_control = NULL; 
            delete _wrappers[i]; 
        }
        _wrappers.clear(); 
    }
}

/**
 * @brief 读取数据
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @param ptr 输出参数，用于存储读取到的数据
 * @return int 成功返回 0，失败返回 -1
 */
template <typename T, typename TLS>
int DoublyBufferedData<T, TLS>::Read(
    typename DoublyBufferedData<T, TLS>::ScopedPtr* ptr) {
    if (!_created_key) {
        return -1; 
    }
    Wrapper* w = static_cast<Wrapper*>(pthread_getspecific(_wrapper_key)); 
    if (w != NULL) {
        w->BeginRead(); 
        ptr->_data = UnsafeRead(); 
        ptr->_w = w; 
        return 0; 
    }
    w = AddWrapper(); 
    if (w != NULL) {
        const int rc = pthread_setspecific(_wrapper_key, w); 
        if (rc == 0) {
            w->BeginRead(); 
            ptr->_data = UnsafeRead(); 
            ptr->_w = w; 
            return 0; 
        }
    }
    return -1; 
}

/**
 * @brief 修改数据
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @tparam Fn 函数类型
 * @param fn 修改数据的函数
 * @return size_t 函数返回值
 */
template <typename T, typename TLS>
template <typename Fn>
size_t DoublyBufferedData<T, TLS>::Modify(Fn fn) {
    std::lock_guard<std::mutex> lock(_modify_mutex); 
    int bg_index = !_index.load(std::memory_order_relaxed); 
    
    const size_t ret = fn(_data[bg_index]); 
    if (!ret) {
        return 0; 
    }

    _index.store(bg_index, std::memory_order_release); 
    bg_index = !bg_index; 
    
    { 
        std::lock_guard<std::mutex> lock(_wrappers_mutex); 
        for (size_t i = 0; i < _wrappers.size(); ++i) {
            _wrappers[i]->WaitReadDone(); 
        }
    }

    const size_t ret2 = fn(_data[bg_index]); 
    assert(ret2 == ret); 
    return ret2; 
}

/**
 * @brief 修改数据（带一个参数）
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @tparam Fn 函数类型
 * @tparam Arg1 参数类型
 * @param fn 修改数据的函数
 * @param arg1 函数参数
 * @return size_t 函数返回值
 */
template <typename T, typename TLS>
template <typename Fn, typename Arg1>
size_t DoublyBufferedData<T, TLS>::Modify(Fn fn, const Arg1& arg1) {
    Closure1<Fn, Arg1> c(fn, arg1); 
    return Modify(c); 
}

/**
 * @brief 修改数据（带两个参数）
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @tparam Fn 函数类型
 * @tparam Arg1 第一个参数类型
 * @tparam Arg2 第二个参数类型
 * @param fn 修改数据的函数
 * @param arg1 第一个函数参数
 * @param arg2 第二个函数参数
 * @return size_t 函数返回值
 */
template <typename T, typename TLS>
template <typename Fn, typename Arg1, typename Arg2>
size_t DoublyBufferedData<T, TLS>::Modify(
    Fn fn, const Arg1& arg1, const Arg2& arg2) {
    Closure2<Fn, Arg1, Arg2> c(fn, arg1, arg2); 
    return Modify(c); 
}

/**
 * @brief 使用前台数据修改数据
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @tparam Fn 函数类型
 * @param fn 修改数据的函数
 * @return size_t 函数返回值
 */
template <typename T, typename TLS>
template <typename Fn>
size_t DoublyBufferedData<T, TLS>::ModifyWithForeground(Fn fn) {
    WithFG0<Fn> c(fn, _data); 
    return Modify(c); 
}

/**
 * @brief 使用前台数据修改数据（带一个参数）
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @tparam Fn 函数类型
 * @tparam Arg1 参数类型
 * @param fn 修改数据的函数
 * @param arg1 函数参数
 * @return size_t 函数返回值
 */
template <typename T, typename TLS>
template <typename Fn, typename Arg1>
size_t DoublyBufferedData<T, TLS>::ModifyWithForeground(Fn fn, const Arg1& arg1) {
    WithFG1<Fn, Arg1> c(fn, _data, arg1); 
    return Modify(c); 
}

/**
 * @brief 使用前台数据修改数据（带两个参数）
 * 
 * @tparam T 数据类型
 * @tparam TLS 线程本地存储类型
 * @tparam Fn 函数类型
 * @tparam Arg1 第一个参数类型
 * @tparam Arg2 第二个参数类型
 * @param fn 修改数据的函数
 * @param arg1 第一个函数参数
 * @param arg2 第二个函数参数
 * @return size_t 函数返回值
 */
template <typename T, typename TLS>
template <typename Fn, typename Arg1, typename Arg2>
size_t DoublyBufferedData<T, TLS>::ModifyWithForeground(
    Fn fn, const Arg1& arg1, const Arg2& arg2) {
    WithFG2<Fn, Arg1, Arg2> c(fn, _data, arg1, arg2); 
    return Modify(c); 
}

}  // namespace butil

#endif  // BUTIL_DOUBLY_BUFFERED_DATA_H
