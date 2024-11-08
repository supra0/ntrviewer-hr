#pragma once

template <typename T>
struct com_ptr {
    T *ptr = nullptr;

    com_ptr(std::nullptr_t = nullptr) {}
    com_ptr(T *ptr) : ptr(ptr) {}

    T **put() { if (ptr) { ptr->Release(); ptr = nullptr; } return &ptr; }
    T *get() const { return ptr; }
    T *operator->() const { return ptr; }
    explicit operator bool() const { return ptr; }
    void copy_from(T* _ptr) { if (ptr) { ptr->Release(); } ptr = _ptr; }
    template <typename To> auto try_as() const {
        To *to;
        HRESULT hr = ptr->QueryInterface(IID_PPV_ARGS(&to));
        if (hr == S_OK) {
            return to;
        }
        return (To *)nullptr;
    }

    ~com_ptr() { if (ptr) { ptr->Release(); } }
};

struct unique_handle {
    HANDLE hdl = 0;

    unique_handle(HANDLE hdl) : hdl(hdl) {}

    HANDLE get() const { return hdl; }
    explicit operator bool() const { return hdl; }
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
