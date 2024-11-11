#pragma once

#include <stdio.h>

template <typename T>
struct com_ptr {
    T *ptr = nullptr;

    com_ptr(std::nullptr_t = nullptr) {}
    com_ptr(T *ptr) : ptr(ptr) { if (ptr) { ptr->AddRef(); } }
    com_ptr(const com_ptr<T> &_ptr) : ptr(_ptr.ptr) { if (ptr) { ptr->AddRef(); } }
    com_ptr(com_ptr<T> &&_ptr) : ptr(_ptr.ptr) { _ptr.ptr = nullptr; }

    T **put() { if (ptr) { ptr->Release(); ptr = nullptr; } return &ptr; }
    T *get() const { return ptr; }
    T *operator->() const { return ptr; }
    explicit operator bool() const { return ptr; }
    void copy_from(T* _ptr) { if (ptr) { ptr->Release(); } ptr = _ptr; if (ptr) { ptr->AddRef(); } }
    template <typename To> auto try_as() const {
        com_ptr<To> to;
        ptr->QueryInterface(IID_PPV_ARGS(to.put()));
        return to;
    }
    com_ptr<T> &operator=(std::nullptr_t) { if (ptr) { ptr->Release(); ptr = nullptr; } return *this; }
    com_ptr<T> &operator=(const com_ptr<T> &_ptr) { if (ptr) { ptr->Release(); } ptr = _ptr.ptr; if (ptr) { ptr->AddRef(); } return *this; }
    com_ptr<T> &operator=(com_ptr<T> &&_ptr) { if (ptr) { ptr->Release(); } ptr = _ptr.ptr; _ptr.ptr = nullptr; return *this; }

    ~com_ptr() { if (ptr) { ptr->Release(); } }
};

struct unique_handle {
    HANDLE hdl = nullptr;

    unique_handle(HANDLE hdl = nullptr) : hdl(hdl) {}

    HANDLE get() const { return hdl; }
    HANDLE *put() { if (hdl) { CloseHandle(hdl); hdl = nullptr; } return &hdl; }
    explicit operator bool() const { return hdl; }

    ~unique_handle() { if (hdl) CloseHandle(hdl); }
};

struct unique_find {
    HANDLE hdl = nullptr;

    unique_find(HANDLE hdl = nullptr) : hdl(hdl) {}

    HANDLE get() const { return hdl; }
    HANDLE *put() { if (hdl) { FindClose(hdl); hdl = nullptr; } return &hdl; }
    explicit operator bool() const { return hdl; }

    ~unique_find() { if (hdl) FindClose(hdl); }
};

struct unique_file {
    FILE *file = nullptr;

    unique_file(FILE *file = nullptr) : file(file) {}

    FILE *get() const { return file; }
    FILE **put() { if (file) { fclose(file); file = nullptr; } return &file; }
    explicit operator bool() const { return file; }

    ~unique_file() { if (file) fclose(file); }
};

struct srwlock {
    SRWLOCK lock = SRWLOCK_INIT;

    struct srwlock_locked_exclusive {
        PSRWLOCK locked;
        ~srwlock_locked_exclusive() {
            ReleaseSRWLockExclusive(locked);
        }
    };

    srwlock_locked_exclusive lock_exclusive() {
        AcquireSRWLockExclusive(&lock);
        return { &lock };
    }
};
