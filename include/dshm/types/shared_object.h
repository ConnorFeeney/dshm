#pragma once

#include "dshm/shared_heap.h"

namespace dshm {

template<typename T>
class shared_object {
public:
    struct from_address_t {
        explicit from_address_t() = default;
    };

    static constexpr from_address_t from_address{};

    template<typename... Args>
    shared_object(shared_heap* heap, std::string name, Args&&... args) : heap(heap), name(name) {
        if (!heap) {
            return;
        }
        this->address = heap->allocate<T>(std::forward<Args>(args)...);
        heap->name_addr(this->address, name);
    }

    shared_object(shared_heap* heap, std::size_t address, std::string name, from_address_t) : heap(heap), address(address) , name(name) {
        if(this->address == 0) {
            return;
        }
        heap->name_addr(this->address, name);
    }

    ~shared_object() = default;

    void destroy() {
        if (!heap || this->address == 0) {
            return;
        }

        if (!this->name.empty()) {
            heap->unname_addr(this->name);
        }
        heap->free(this->address);
        
        this->heap = nullptr;
        this->address = 0;
    }

    std::size_t addr() const {
        return this->address;
    }

    operator T() const {
        if (!heap || this->address == 0) {
            return T{};
        }

        return heap->read<T>(this->address);
    }

    shared_object& operator=(const T& value) {
        if (!heap || this->address == 0) {
            return *this;
        }

        heap->write<T>(this->address, value);
        return *this;
    }

    template<typename U>
    bool operator==(const shared_object<U>& other) const {
        static_assert(std::is_same<T, U>::value, "Types must be the same");
        const T& thisValue = static_cast<T>(*this);
        const T& otherValue = static_cast<T>(other);
        return thisValue == otherValue;
    }

    T operator+(const T& value) requires std::is_integral_v<T> {
        if (!heap || this->address == 0) {
            return T{};
        }
        return heap->read<T>(this->address) + value;
    }

    shared_object& operator+=(const T& value) requires std::is_integral_v<T> {
        if (!heap || this->address == 0) {
            return *this;
        }
        heap->fetch_add<T>(this->address, value);
        return *this;
    }

    shared_object& operator++() requires std::is_integral_v<T> {
        if (!heap || this->address == 0) {
            return *this;
        }
        heap->fetch_add<T>(this->address, 1);
        return *this;
    }

    T operator++(int) requires std::is_integral_v<T> {
        if (!heap || this->address == 0) return T{};
        T old = heap->read<T>(this->address);
        heap->fetch_add<T>(this->address, 1);
        return old;
    }

    T operator-(const T& value) requires std::is_integral_v<T> {
        if (!heap || this->address == 0) {
            return T{};
        }
        return heap->read<T>(this->address) - value;
    }

    shared_object& operator-=(const T& value) requires std::is_integral_v<T> {
        if (!heap || this->address == 0) {
            return *this;
        }
        heap->fetch_add<T>(this->address, -value);
        return *this;
    }

    shared_object& operator--() requires std::is_integral_v<T> {
        if (!heap || this->address == 0) {
            return *this;
        }
        heap->fetch_add<T>(this->address, -1);
        return *this;
    }

    T operator--(int) requires std::is_integral_v<T> {
        if (!heap || this->address == 0) return T{};
        T old = heap->read<T>(this->address);
        heap->fetch_add<T>(this->address, -1);
        return old;
    }

private:
    shared_heap* heap;
    std::size_t address;
    std::string name;
};

template<typename T>
shared_object<T> dshm_make_or_find(std::string sharedHeap, std::string name) {
    shared_heap* heap = sheap(sharedHeap);
    std::size_t searchAddr = heap->find(name);
    if (searchAddr == 0) {
        shared_object<T> res(heap, name);
        return res;
    }

    shared_object<T> res(heap, searchAddr, name, shared_object<T>::from_address);
    return res;
}

}