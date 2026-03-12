#pragma once

#include "dshm/shared_heap.h"

template<typename T>
class shared_object {
public:
    struct from_address_t {
        explicit from_address_t() = default;
    };

    static constexpr from_address_t from_address{};

    template<typename... Args>
    shared_object(shared_heap* heap, Args&&... args) : heap(heap) {
        if (!heap) {
            return;
        }
        this->address = heap->allocate<T>(std::forward<Args>(args)...);
    }

    shared_object(shared_heap* heap, std::size_t address, from_address_t) : heap(heap), address(address) {}

    ~shared_object() {
        if (!heap) {
            return;
        }

        if (!this->name.empty()) {
            heap->unname_addr(this->name);
        }
        heap->free(this->address);
    }

    std::size_t addr() const {
        return this->address;
    }

    void set_name(std::string& name) {
        this->name = name;
    }

    operator T() const {
        if (!heap) {
            return T{};
        }

        return heap->read<T>(this->address);
    }

    shared_object& operator=(const T& value) {
        if (!heap) {
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
        shared_object<T> res(heap);
        res.set_name(name);
        heap->name_addr(res.addr(), name);
        return res;
    }

    shared_object<T> res(heap, searchAddr, shared_object<T>::from_address);
    res.set_name(name);
    return res;
}