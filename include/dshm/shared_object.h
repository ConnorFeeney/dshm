#pragma once

#include "dshm/shared_heap.h"
#include <unordered_map>

template<typename T>
class shared_object {
public:
    template<typename... Args>
    shared_object(shared_heap* heap, Args&&... args) : heap(heap) {
        if (!heap) {
            return;
        }
        static_assert(std::is_trivially_copyable<T>::value, "Shared object type must be trivally copyable");
        this->address = heap->allocate<T>(std::forward<Args>(args)...);
    }

    ~shared_object() {
        if (!heap) {
            return;
        }
        heap->free(this->address);
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