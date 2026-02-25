#pragma once

#include <cstdint>

template<typename T>
class Flags {
public:
    constexpr Flags() : values(0) {}
    constexpr Flags(unsigned n) : values{1ULL << n} {}

    constexpr Flags operator| (const Flags& other) const {
        return {this->values | other.values, 0};
    }

    constexpr bool operator& (const Flags& other) const {
        return this->values & other.values;
    }
private:
    std::uint64_t values;
    constexpr Flags(std::uint64_t v, int) : values{v} {}
};

#define DECLARE_FLAGS(name) \
    class name##__TAG; \
    using name = Flags<name##__TAG>;

#define DEFINE_FLAGS(name, option, index) \
    constexpr name option(index);