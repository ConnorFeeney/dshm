#pragma once

#include "dshm/shm.h"
#include <unordered_map>


typedef struct heap_header {
    pthread_mutex_t heapMutex;
    std::atomic<std::int64_t> block;
} heap_header;

typedef struct block_header {
    std::atomic<std::size_t> meta;
    std::atomic<std::int64_t> next;
    std::atomic<std::int64_t> prev;
} blockHeader;

static constexpr std::size_t K_BLOCK_ALLOCATED = 1UL << 0;
static constexpr std::size_t K_BLOCK_ATOMIC = 1UL << 1;
static constexpr std::size_t K_BLOCK_ATOMIC_REF = 1UL << 2;
static constexpr std::size_t K_BLOCK_FLAG_MASK = K_BLOCK_ALLOCATED | K_BLOCK_ATOMIC | K_BLOCK_ATOMIC_REF;

static std::size_t align8_offset(const void* p) {
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    std::size_t rem = v & 0x7;
    return rem ? (8 - rem) : 0;
}

static std::size_t pack_block_meta(std::size_t size, std::size_t flags) {
    size = round8_down(size);
    return (size & ~K_BLOCK_FLAG_MASK) | (flags & K_BLOCK_FLAG_MASK);
}

static std::size_t unpack_block_size(std::size_t meta) {
    std::size_t dat = meta & ~K_BLOCK_FLAG_MASK;
    return dat;
}

static std::size_t unpack_block_flags(std::size_t meta) {
    return meta & K_BLOCK_FLAG_MASK;
}

class shared_heap {
public:
    shared_heap(std::string name) {
        memory = create_shm(name, &this->sstat);
        if (!memory) {
            return;
        }
        this->name = name;

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        if (shm_stat_creator(&this->sstat)) {
            pthread_mutexattr_t attr;
            if (pthread_mutexattr_init(&attr) != 0) {
                return;
            }
            if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
                pthread_mutexattr_destroy(&attr);
                return;
            }
            if (pthread_mutex_init(&head->heapMutex, &attr) != 0) {
                pthread_mutexattr_destroy(&attr);
                return;
            }
            pthread_mutexattr_destroy(&attr);
        }

        std::size_t nextf = sizeof(heap_header) + align8_offset(reinterpret_cast<char*>(head) + sizeof(heap_header));
        block_header* free = reinterpret_cast<block_header*>(reinterpret_cast<char*>(this->memory) + nextf);
        free->next.store(-1, std::memory_order_release);
        free->prev.store(-1, std::memory_order_release);
        std::size_t blockSize = round8_down(sstat.size - nextf);
        free->meta.store(pack_block_meta(blockSize, 0), std::memory_order_release);

        head->block.store(nextf, std::memory_order_release);
    }

    ~shared_heap() {
        if (shm_stat_owner(&this->sstat)) {
            unlink_shm(&this->memory, this->name, &this->sstat);
        }

        unmap_shm(&this->memory, &this->sstat);
    }

    template<typename T, typename... Args>
    std::size_t allocate(Args... args) {
        if (!verify_shm(&this->memory, &sstat)) {
            return 0;
        }
        const bool lockFree = std::atomic<T>::is_always_lock_free;
        const std::size_t objSize = lockFree ? sizeof(std::atomic<T>) : sizeof(T);
        block_header* block = nullptr;
        std::size_t blockAddr = get_block(&block, objSize);
        if (block == nullptr) {
            return 0;
        }
        char* blockStart = reinterpret_cast<char*>(this->memory) + blockAddr;
        char* objPtr = blockStart + sizeof(block_header);
        objPtr += align8_offset(objPtr);

        std::size_t flags = K_BLOCK_ALLOCATED;
        if (lockFree) {
            flags |= K_BLOCK_ATOMIC;
        }

        std::size_t blockSize = unpack_block_size(block->meta.load(std::memory_order_acquire));
        block->meta.store(pack_block_meta(blockSize, flags), std::memory_order_release);

        if (lockFree) {
            T initial(std::forward<Args>(args)...);
            ::new (objPtr) std::atomic<T>(initial);
        } else {
            heap_header* head = reinterpret_cast<heap_header*>(this->memory);
            pthread_mutex_lock(&head->heapMutex);
            if (errno == EOWNERDEAD) {
                pthread_mutex_consistent(&head->heapMutex);
            }
            ::new (objPtr) T(std::forward<Args>(args)...);
            pthread_mutex_unlock(&head->heapMutex);
        }

        return static_cast<std::size_t>(objPtr - reinterpret_cast<char*>(this->memory));
    }

    template<typename T>
    std::size_t allocate_array(std::size_t size) {
        if (!verify_shm(&this->memory, &sstat)) {
            return 0;
        }
        const bool lockFree = std::atomic<T>::is_always_lock_free;
        const std::size_t arraySizeBytes = sizeof(T) * size;
        block_header* block = nullptr;
        std::size_t blockAddr = get_block(&block, arraySizeBytes);
        if (block == nullptr) {
            return 0;
        }

        char* blockStart = reinterpret_cast<char*>(this->memory) + blockAddr;
        char* objPtr = blockStart + sizeof(block_header);
        objPtr += align8_offset(objPtr);

        std::size_t flags = K_BLOCK_ALLOCATED;
        if (lockFree) {
            flags |= K_BLOCK_ATOMIC_REF;
        }

        std::size_t blockSize = unpack_block_size(block->meta.load(std::memory_order_acquire));
        block->meta.store(pack_block_meta(blockSize, flags), std::memory_order_release);

        return static_cast<std::size_t>(objPtr - reinterpret_cast<char*>(this->memory));
    }

    bool free(std::size_t addr) {
        if (!verify_shm(&this->memory, &sstat)) {
            return false;
        }

        char* base = reinterpret_cast<char*>(this->memory);
        if (addr == 0) {
            return false;
        }

        char* objPtr = base + addr;
        char* blockStart = reinterpret_cast<char*>(round8_down(reinterpret_cast<std::uintptr_t>(objPtr - sizeof(block_header))));

        std::size_t blockAddr = static_cast<std::size_t>(blockStart - base);
        block_header* block = reinterpret_cast<block_header*>(blockStart);
        std::size_t blockMeta = block->meta.load(std::memory_order_acquire);
        std::size_t blockFlags = unpack_block_flags(blockMeta);
        if ((blockFlags & K_BLOCK_ALLOCATED) == 0) {
            return false;
        }

        std::size_t blockSize = unpack_block_size(blockMeta);

        std::int64_t nextOffset = block->next.load(std::memory_order_acquire);
        if (nextOffset != -1) {
            block_header* nextBlock = reinterpret_cast<block_header*>(base + nextOffset);
            std::size_t nextMeta = nextBlock->meta.load(std::memory_order_acquire);
            if ((unpack_block_flags(nextMeta) & K_BLOCK_ALLOCATED) == 0) {
                std::size_t nextSize = unpack_block_size(nextMeta);
                std::int64_t nextNext = nextBlock->next.load(std::memory_order_acquire);

                blockSize += nextSize;
                block->next.store(nextNext, std::memory_order_release);
                if (nextNext != -1) {
                    block_header* nextNextBlock = reinterpret_cast<block_header*>(base + nextNext);
                    nextNextBlock->prev.store(blockAddr, std::memory_order_release);
                }
            }
        }

        std::int64_t prevOffset = block->prev.load(std::memory_order_acquire);
        if (prevOffset != -1) {
            block_header* prevBlock = reinterpret_cast<block_header*>(base + prevOffset);
            std::size_t prevMeta = prevBlock->meta.load(std::memory_order_acquire);
            if ((unpack_block_flags(prevMeta) & K_BLOCK_ALLOCATED) == 0) {
                std::size_t prevSize = unpack_block_size(prevMeta);
                std::int64_t nextAfter = block->next.load(std::memory_order_acquire);

                blockSize += prevSize;
                prevBlock->next.store(nextAfter, std::memory_order_release);
                if (nextAfter != -1) {
                    block_header* nextBlock = reinterpret_cast<block_header*>(base + nextAfter);
                    nextBlock->prev.store(prevOffset, std::memory_order_release);
                }

                block = prevBlock;
            }
        }

        block->meta.store(pack_block_meta(blockSize, 0), std::memory_order_release);
        return true;
    }

    template<typename T>
    T read(std::size_t addr) {
        if (!verify_shm(&this->memory, &sstat)) {
            return T{};
        }

        char* base = reinterpret_cast<char*>(this->memory);
        char* objPtr = base + addr;
        char* blockStart = reinterpret_cast<char*>(round8_down(reinterpret_cast<std::uintptr_t>(objPtr - sizeof(block_header))));

        block_header* block = reinterpret_cast<block_header*>(blockStart);
        

        return this->read<T>(objPtr, block);
    }

    template<typename T>
    T read_index(std::size_t addr, std::size_t index) {
        if (!verify_shm(&this->memory, &sstat)) {
            return T{};
        }

        char* base = reinterpret_cast<char*>(this->memory);
        char* baseObjPtr = base + addr;
        char* blockStart = reinterpret_cast<char*>(round8_down(reinterpret_cast<std::uintptr_t>(baseObjPtr - sizeof(block_header))));

        char* objPtr = baseObjPtr + (index * sizeof(T));
        block_header* block = reinterpret_cast<block_header*>(blockStart);
        

        return this->read<T>(objPtr, block);
    }

    template<typename T>
    bool write(std::size_t addr, const T& val) {
        if (!verify_shm(&this->memory, &sstat)) {
            return false;
        }

        char* base = reinterpret_cast<char*>(this->memory);
        char* objPtr = base + addr;
        char* blockStart = reinterpret_cast<char*>(round8_down(reinterpret_cast<std::uintptr_t>(objPtr - sizeof(block_header))));

        block_header* block = reinterpret_cast<block_header*>(blockStart);
        
        this->write<T>(objPtr, block, val);

        return true;
    }

    template<typename T>
    bool write_index(std::size_t addr, std::size_t index, const T& val) {
        if (!verify_shm(&this->memory, &sstat)) {
            return false;
        }

        char* base = reinterpret_cast<char*>(this->memory);
        char* baseObjPtr = base + addr;
        char* blockStart = reinterpret_cast<char*>(round8_down(reinterpret_cast<std::uintptr_t>(baseObjPtr - sizeof(block_header))));

        char* objPtr = baseObjPtr + (index * sizeof(T));
        block_header* block = reinterpret_cast<block_header*>(blockStart);

        this->write<T>(objPtr, block, val);
        return true;
    }

private:
    template<typename T>
    void write(char* objPtr, block_header* block,  const T& val) {
        const std::uint64_t blockMeta = block->meta.load(std::memory_order_acquire);
        const std::uint64_t blockFlags = unpack_block_flags(blockMeta);

        const bool isAtomic = blockFlags & K_BLOCK_ATOMIC;
        const bool isAtomicRef = blockFlags & K_BLOCK_ATOMIC_REF;

        if (isAtomic) {
            auto* obj = reinterpret_cast<std::atomic<T>*>(objPtr);
            obj->store(val, std::memory_order_release);
        } else if (isAtomicRef) {
            std::atomic_ref<T> ref(*reinterpret_cast<T*>(objPtr));
            ref.store(val, std::memory_order_release);
        } else {
            heap_header* head = reinterpret_cast<heap_header*>(this->memory);
            pthread_mutex_lock(&head->heapMutex);
            if (errno == EOWNERDEAD) {
                pthread_mutex_consistent(&head->heapMutex);
            }
            *reinterpret_cast<T*>(objPtr) = val;
            pthread_mutex_unlock(&head->heapMutex);
        }
    }

    template<typename T>
    T read(char* objPtr, block_header* block) {
        const std::uint64_t blockMeta = block->meta.load(std::memory_order_acquire);
        const std::uint64_t blockFlags = unpack_block_flags(blockMeta);

        const bool isAtomic = blockFlags & K_BLOCK_ATOMIC;
        const bool isAtomicRef = blockFlags & K_BLOCK_ATOMIC_REF;

        T value{};
        if (isAtomic) {
            auto* obj = reinterpret_cast<std::atomic<T>*>(objPtr);
            value = obj->load(std::memory_order_acquire);
        } else if (isAtomicRef) {
            std::atomic_ref<T> ref(*reinterpret_cast<T*>(objPtr));
            value = ref.load(std::memory_order_acquire);
        } else {
            heap_header* head = reinterpret_cast<heap_header*>(this->memory);
            pthread_mutex_lock(&head->heapMutex);
            if (errno == EOWNERDEAD) {
                pthread_mutex_consistent(&head->heapMutex);
            }
            value = *reinterpret_cast<T*>(objPtr);
            pthread_mutex_unlock(&head->heapMutex);
        }

        return value;
    }

    std::size_t allocation_size(char* blockStart, std::size_t objSize) const {
        std::size_t offset = sizeof(block_header);
        // Keep payload 8-byte aligned for shared memory safety.
        offset += align8_offset(blockStart + offset);
        offset += objSize;
        offset += align8_offset(blockStart + offset);
        if (offset < sizeof(block_header)) {
            offset = sizeof(block_header);
        }
        return offset;
    }

    std::size_t get_block(block_header** blk, std::size_t size) {
        if (!verify_shm(&this->memory, &sstat)) {
            *blk = nullptr;
            return 0;
        }

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        char* base = reinterpret_cast<char*>(this->memory);

        std::size_t blockAddress = head->block.load(std::memory_order_acquire);
        block_header* block = reinterpret_cast<block_header*>(base + blockAddress);

        std::size_t blockMeta = block->meta.load(std::memory_order_acquire);
        std::size_t blockFlags = unpack_block_flags(blockMeta);

        std::size_t requiredSize = allocation_size(reinterpret_cast<char*>(block), size);

        while (blockFlags & K_BLOCK_ALLOCATED || unpack_block_size(blockMeta) < requiredSize) {
            std::size_t next = block->next.load(std::memory_order_acquire);
            if (next == -1) {
                std::size_t required = this->sstat.size + requiredSize;
                std::size_t newSize = 2 * this->sstat.size > required ? 2* this->sstat.size : required;
                if (resize_shm(&this->memory, &sstat, newSize) == 0) {
                    *blk = nullptr;
                    return 0;
                }

                std::size_t nBlockAddress = blockAddress + unpack_block_size(blockMeta);
                block_header* nBlock = reinterpret_cast<block_header*>(base + nBlockAddress);
                std::size_t nBlockSize = round8_down(this->sstat.size - nBlockAddress);

                nBlock->next.store(-1, std::memory_order_release);
                nBlock->prev.store(blockAddress, std::memory_order_release);
                nBlock->meta.store(pack_block_meta(nBlockSize, 0), std::memory_order_release);

                block->next.store(nBlockAddress, std::memory_order_release);

                next = nBlockAddress;
            }

            block = reinterpret_cast<block_header*>(base + next);
            blockAddress = next;

            blockMeta = block->meta.load(std::memory_order_acquire);
            blockFlags = unpack_block_flags(blockMeta);

            requiredSize = allocation_size(reinterpret_cast<char*>(block), size);
        }

        std::size_t blockSize = unpack_block_size(blockMeta);
        std::size_t remaining = blockSize - requiredSize;
        if (remaining >= requiredSize) {
            std::size_t nBlockSize = round8_down(remaining);
            std::size_t nBlockAddress = blockAddress + requiredSize;
            std::size_t nBlockMeta = pack_block_meta(nBlockSize, 0);

            block_header* nBlock = reinterpret_cast<block_header*>(base + nBlockAddress);

            nBlock->prev.store(blockAddress, std::memory_order_release);
            nBlock->next.store(block->next.load(std::memory_order_acquire), std::memory_order_release);
            nBlock->meta.store(nBlockMeta, std::memory_order_release);

            std::int64_t next = nBlock->next.load(std::memory_order_acquire);
            if (next != -1) {
                block_header* nextBlock = reinterpret_cast<block_header*>(base + next);
                nextBlock->prev.store(nBlockAddress, std::memory_order_release);
            }

            blockMeta = pack_block_meta(requiredSize, 0);
            block->meta.store(blockMeta, std::memory_order_release);
            block->next.store(nBlockAddress, std::memory_order_release);
        }

        *blk = block;
        return blockAddress;
    }

    void* memory;
    shm_stat sstat;
    std::string name;
};

inline shared_heap* sheap(std::string name) {
    static std::unordered_map<std::string, shared_heap> heaps;
    auto [it, inserted] = heaps.try_emplace(name, name);
    return &it->second;
}