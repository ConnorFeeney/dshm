#pragma once

#include "dshm/shared_heap.h"
#include <unordered_map>

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
        static_assert(std::is_trivially_copyable<T>::value, "Shared object type must be trivally copyable");
        this->address = heap->allocate<T>(std::forward<Args>(args)...);
    }

    shared_object(shared_heap* heap, std::size_t address, from_address_t) : heap(heap), address(address) {}

    ~shared_object() {
        if (!heap) {
            return;
        }
        heap->free(this->address);
    }

    std::size_t addr() const {
        return this->address;
    }

    operator T() const {
        if (!heap) {
            return T{};
        }

        return heap->read_at<T>(this->address);
    }

    shared_object& operator=(const T& value) {
        if (!heap) {
            return *this;
        }

        heap->write_at<T>(this->address, value);
        return *this;
    }

    template<typename U>
    bool operator==(const shared_object<U>& other) const {
        static_assert(std::is_same<T, U>::value, "Types must be the same");
        T thisValue = static_cast<T>(*this);
        T otherValue = static_cast<T>(other);
        return thisValue == otherValue;
    }

private:
    shared_heap* heap;
    std::size_t address;
};

static shared_heap* heap(std::string name) {
    static std::unordered_map<std::string, shared_heap> heaps;
    std::error_code ec;
    auto [it, inserted] = heaps.try_emplace(name, name, ec);
    return &it->second;
}

template<typename T, typename... Args>
shared_object<T> make_shared_obj(std::string sharedNameSpace, Args&&... args) {
    return shared_object<T>(heap(sharedNameSpace), std::forward<Args>(args)...);
}

template<typename T>
shared_object<T> shared_from_address(std::string sharedNameSpace, std::size_t address) {
    return shared_object<T>(heap(sharedNameSpace), address, shared_object<T>::from_address);
}