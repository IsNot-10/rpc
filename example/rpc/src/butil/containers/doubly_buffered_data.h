#pragma once

#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <memory>
#include <thread>
#include <cassert>

namespace butil {

template <typename T>
class DoublyBufferedData {
private:
    struct Wrapper;

public:
    class ScopedPtr {
    public:
        ScopedPtr() : _data(nullptr), _wrapper(nullptr) {}
        ~ScopedPtr() {
            if (_wrapper) {
                _wrapper->_mutex.unlock();
            }
        }
        
        const T* get() const { return _data; }
        const T& operator*() const { return *_data; }
        const T* operator->() const { return _data; }
        
        // Allow moving
        ScopedPtr(ScopedPtr&& other) : _data(other._data), _wrapper(other._wrapper) {
            other._data = nullptr;
            other._wrapper = nullptr;
        }
        
        ScopedPtr& operator=(ScopedPtr&& other) {
            if (this != &other) {
                if (_wrapper) _wrapper->_mutex.unlock();
                _data = other._data;
                _wrapper = other._wrapper;
                other._data = nullptr;
                other._wrapper = nullptr;
            }
            return *this;
        }
        
    private:
        // No copy
        ScopedPtr(const ScopedPtr&) = delete;
        ScopedPtr& operator=(const ScopedPtr&) = delete;

        friend class DoublyBufferedData;
        const T* _data;
        Wrapper* _wrapper;
    };

    DoublyBufferedData() : _index(0) {}
    
    ~DoublyBufferedData() {
        // Clear wrappers to prevent access to destroyed parent
        std::lock_guard<std::mutex> lock(_wrappers_mutex);
        for (auto* w : _wrappers) {
            w->_parent = nullptr; // Detach
        }
    }

    int Read(ScopedPtr* ptr) {
        Wrapper* w = GetThreadLocalWrapper();
        if (!w) return -1;
        
        w->_mutex.lock();
        ptr->_data = &_data[_index.load(std::memory_order_acquire)];
        ptr->_wrapper = w;
        return 0;
    }

    template <typename Fn>
    size_t Modify(Fn fn) {
        std::lock_guard<std::mutex> lock(_modify_mutex);
        int bg_index = !_index.load(std::memory_order_relaxed);
        
        // 1. Modify background
        size_t ret = fn(_data[bg_index]);
        
        // 2. Flip
        _index.store(bg_index, std::memory_order_release);
        
        // 3. Wait for readers
        {
            std::lock_guard<std::mutex> wl(_wrappers_mutex);
            for (auto* w : _wrappers) {
                std::lock_guard<std::mutex> l(w->_mutex);
            }
        }
        
        // 4. Modify new background (old foreground)
        fn(_data[!bg_index]);
        
        return ret;
    }
    
    template <typename Fn>
    size_t ModifyWithForeground(Fn fn) {
         std::lock_guard<std::mutex> lock(_modify_mutex);
        int bg_index = !_index.load(std::memory_order_relaxed);
        int fg_index = !bg_index;
        
        // 1. Modify background using foreground as reference
        size_t ret = fn(_data[bg_index], (const T&)_data[fg_index]);
        
        // 2. Flip
        _index.store(bg_index, std::memory_order_release);
        
        // 3. Wait for readers
        {
            std::lock_guard<std::mutex> wl(_wrappers_mutex);
            for (auto* w : _wrappers) {
                std::lock_guard<std::mutex> l(w->_mutex);
            }
        }
        
        // 4. Modify new background
        fn(_data[fg_index], (const T&)_data[bg_index]);
        
        return ret;
    }
    
    template <typename Fn, typename Arg>
    size_t Modify(Fn fn, const Arg& arg) {
        auto wrapper = [&](T& t) { return fn(t, arg); };
        return Modify(wrapper);
    }
    
    template <typename Fn, typename Arg>
    size_t ModifyWithForeground(Fn fn, const Arg& arg) {
        auto wrapper = [&](T& bg, const T& fg) { return fn(bg, fg, arg); };
        return ModifyWithForeground(wrapper);
    }

private:
    struct Wrapper {
        std::mutex _mutex;
        DoublyBufferedData* _parent;
        
        Wrapper(DoublyBufferedData* p) : _parent(p) {}
        ~Wrapper() {
            if (_parent) _parent->RemoveWrapper(this);
        }
    };

    void RemoveWrapper(Wrapper* w) {
        std::lock_guard<std::mutex> lock(_wrappers_mutex);
        auto it = std::find(_wrappers.begin(), _wrappers.end(), w);
        if (it != _wrappers.end()) {
            _wrappers.erase(it);
        }
    }

    Wrapper* GetThreadLocalWrapper() {
        static thread_local std::unique_ptr<Wrapper> tls_wrapper;
        if (!tls_wrapper) {
            tls_wrapper.reset(new Wrapper(this));
            std::lock_guard<std::mutex> lock(_wrappers_mutex);
            _wrappers.push_back(tls_wrapper.get());
        }
        return tls_wrapper.get();
    }

    T _data[2];
    std::atomic<int> _index;
    std::mutex _modify_mutex;
    std::vector<Wrapper*> _wrappers;
    std::mutex _wrappers_mutex;
};

} // namespace butil
