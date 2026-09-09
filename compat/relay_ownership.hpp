// Copyright (c) relay12 contributors.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>

// A deliberately small CComPtr-compatible ownership type shared by the two
// Microsoft ports. It models only operations exercised by the pinned sources.
template <typename Interface>
class RelayComPtr
{
public:
    RelayComPtr() noexcept = default;
    RelayComPtr(std::nullptr_t) noexcept {}
    RelayComPtr(Interface* pointer) noexcept : pointer_(pointer) { addRef(); }

    RelayComPtr(const RelayComPtr& other) noexcept : pointer_(other.pointer_)
    {
        addRef();
    }

    RelayComPtr(RelayComPtr&& other) noexcept
        : pointer_(std::exchange(other.pointer_, nullptr))
    {
    }

    ~RelayComPtr() noexcept { release(); }

    RelayComPtr& operator=(const RelayComPtr& other) noexcept
    {
        Interface* incoming = other.pointer_;
        if (incoming)
            incoming->AddRef();
        release();
        pointer_ = incoming;
        return *this;
    }

    RelayComPtr& operator=(RelayComPtr&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            release();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    RelayComPtr& operator=(Interface* pointer) noexcept
    {
        if (pointer)
            pointer->AddRef();
        release();
        pointer_ = pointer;
        return *this;
    }

    RelayComPtr& operator=(std::nullptr_t) noexcept
    {
        release();
        return *this;
    }

    operator Interface*() const noexcept { return pointer_; }
    Interface* get() const noexcept { return pointer_; }
    Interface* operator->() const noexcept
    {
        assert(pointer_);
        return pointer_;
    }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    // CComPtr requires an empty pointer before exposing an output slot. This
    // catches leaked references instead of silently releasing live state.
    Interface** operator&() noexcept
    {
        assert(pointer_ == nullptr);
        return std::addressof(pointer_);
    }

    void Attach(Interface* pointer) noexcept
    {
        if (pointer_ != pointer)
        {
            release();
            pointer_ = pointer;
        }
    }

    Interface* Detach() noexcept
    {
        return std::exchange(pointer_, nullptr);
    }

private:
    void addRef() noexcept
    {
        if (pointer_)
            pointer_->AddRef();
    }

    void release() noexcept
    {
        Interface* old = std::exchange(pointer_, nullptr);
        if (old)
            old->Release();
    }

    Interface* pointer_ = nullptr;
};

// Generic adopted-allocation ownership. The port supplies the allocator's
// exact deleter (for ATL CComHeapPtr this is CoTaskMemFree).
template <typename T, typename Deleter>
class RelayHeapPtr
{
public:
    RelayHeapPtr() noexcept = default;
    RelayHeapPtr(T* pointer) noexcept : pointer_(pointer) {}
    RelayHeapPtr(const RelayHeapPtr&) = delete;
    RelayHeapPtr& operator=(const RelayHeapPtr&) = delete;

    RelayHeapPtr(RelayHeapPtr&& other) noexcept
        : pointer_(std::exchange(other.pointer_, nullptr))
    {
    }

    RelayHeapPtr& operator=(RelayHeapPtr&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            release();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    ~RelayHeapPtr() noexcept { release(); }

    operator T*() const noexcept { return pointer_; }
    T* get() const noexcept { return pointer_; }

    T** operator&() noexcept
    {
        assert(pointer_ == nullptr);
        return std::addressof(pointer_);
    }

    void Attach(T* pointer) noexcept
    {
        if (pointer_ != pointer)
        {
            release();
            pointer_ = pointer;
        }
    }

    T* Detach() noexcept { return std::exchange(pointer_, nullptr); }

private:
    void release() noexcept
    {
        T* old = std::exchange(pointer_, nullptr);
        if (old)
            Deleter{}(old);
    }

    T* pointer_ = nullptr;
};
