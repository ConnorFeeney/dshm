#pragma once

// ISO Includes
#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

// GNU Includes
#include <sys/mman.h>
#include <sys/stat.h> 
#include <fcntl.h> 
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

typedef struct heapHeader {
    std::atomic<std::uint8_t> noResize;
    std::atomic<pid_t> ownerPID;
    std::atomic<std::uint64_t> brk;
    std::atomic<std::int64_t> nextFree;
    std::atomic<std::uint64_t> version;
    pthread_mutex_t heapMutex;
} heapHeader;

struct freeBlockHeader {
    std::atomic<std::int64_t> next;
    std::atomic<std::int64_t> prev;
    std::atomic<std::size_t> size;
};

struct blockHeader {
    std::atomic<std::uint64_t> sizeAndFlags;
};

inline std::size_t align8_offset(const void* p) {
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    std::size_t rem = v & 0x7;
    return rem ? (8 - rem) : 0;
}

inline char* align8_down(char* p) {
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    v &= ~static_cast<std::uintptr_t>(0x7);
    return reinterpret_cast<char*>(v);
}

class shared_heap {
public:
    shared_heap(std::string name, std::error_code& ec) {
        this->isOwner = false;
        if (!atomics_lock_free()) {
            std::cerr << "atomics not lock-free";
            return;
        }
        // Attemp to create a new SHM
        bool newSHM = false;
        this->heapfd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
        if (this->heapfd != -1) {
            newSHM = true;
        } else if(this->heapfd < 0 && errno == EEXIST) {
            this->heapfd = shm_open(name.c_str(), O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
            if (this->heapfd == -1) {
                std::cerr << "shm old";
                return;
            }
        } else {
            std::cerr << "shm";
            return;
        }

        heapHeader* head = nullptr;

        std::size_t pageSize = sysconf(_SC_PAGESIZE);
        std::size_t bufferSize = 0;
        if (!newSHM) {
            struct stat st;
            if(fstat(this->heapfd, &st) < 0) {
                std::cerr << "fstat";
                return;
            }

            bufferSize = st.st_size;
        }

        bool isValidSize = true;
        if (!newSHM) {
            isValidSize = bufferSize >= 2 * pageSize;
            if (!isValidSize) {
                newSHM = true;
            }
        }

        if (!newSHM) {
            head = reinterpret_cast<heapHeader*>(mmap(NULL, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, this->heapfd, 0));
            if (head == MAP_FAILED) {
                std::cerr << "ptr";
                return;
            }

            std::size_t ownerPID = head->ownerPID.load(std::memory_order_acquire);

            bool ownerRunning = false;
            if (kill(ownerPID, 0) == 0) {
                ownerRunning = true;
                this->isOwner = false;
            } else if (errno == EPERM) {
                ownerRunning = true;
                this->isOwner = false;
            }

            if (!ownerRunning) {
                head->ownerPID.store(getpid(), std::memory_order_release);
                head->noResize.store(0, std::memory_order_release);
                this->isOwner = true;
            }

            if(munmap(head, bufferSize) < 0) {
                std::cerr << "munmap no own";
                return;
            }
        }

        if (newSHM) {
            this->isOwner = true;
            if (ftruncate(this->heapfd, 2 * pageSize) < 0) {
                std::cerr << "ftruncate";
                return;
            }
            bufferSize = 2 * pageSize;

            head = reinterpret_cast<heapHeader*>(mmap(NULL, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, this->heapfd, 0));
            if (head == MAP_FAILED) {
                std::cerr << "ptr";
                return;
            }

            head->ownerPID.store(getpid(), std::memory_order_release);
            head->brk.store(bufferSize, std::memory_order_release);

            std::size_t nextf = + sizeof(heapHeader) + align8_offset(reinterpret_cast<char*>(head) + sizeof(heapHeader));
            head->nextFree.store(nextf, std::memory_order_release);

            freeBlockHeader* firstFree = reinterpret_cast<freeBlockHeader*>(reinterpret_cast<char*>(head) + nextf);
            firstFree->next.store(-1, std::memory_order_release);
            firstFree->prev.store(-1, std::memory_order_release);
            firstFree->size.store(bufferSize - nextf, std::memory_order_release);

            head->version.store(0, std::memory_order_release);
            head->noResize.store(0, std::memory_order_release);

            pthread_mutexattr_t attr;
            if (pthread_mutexattr_init(&attr) != 0) {
                std::cerr << "mutexattr init";
                return;
            }
            if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
                std::cerr << "mutexattr pshared";
                pthread_mutexattr_destroy(&attr);
                return;
            }
            if (pthread_mutex_init(&head->heapMutex, &attr) != 0) {
                std::cerr << "mutex init";
                pthread_mutexattr_destroy(&attr);
                return;
            }
            pthread_mutexattr_destroy(&attr);

            if(munmap(head, bufferSize) < 0) {
                std::cerr << "munmap";
                return;
            }
        }

        this->name = name;
        this->memoryp = mmap(NULL, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, this->heapfd, 0);
        if (this->memoryp == MAP_FAILED) {
            std::cerr << "memoryp";
            return;
        }
        this->localVersion = reinterpret_cast<heapHeader*>(this->memoryp)->version.load(std::memory_order_acquire);
        this->localBrk = reinterpret_cast<heapHeader*>(this->memoryp)->brk.load(std::memory_order_acquire);
    }

    ~shared_heap() {
        if (isOwner) {
            reinterpret_cast<heapHeader*>(this->memoryp)->noResize.store(1, std::memory_order_acquire);
            shm_unlink(this->name.c_str());
        }
 
        munmap(this->memoryp, localBrk);
        close(this->heapfd);
    }

    template<typename T, typename... Args>
    std::size_t allocate(Args... args) {
        if constexpr (!std::is_trivially_copyable_v<T>) {
            return 0;
        }

        if (verrifyMapping() < 0) {
            return 0;
        }

        const bool lockFreeObject = std::atomic<T>().is_lock_free();

        for (int attempt = 0; attempt < 2; ++attempt) {
            heapHeader* head = reinterpret_cast<heapHeader*>(this->memoryp);
            std::int64_t nextFreeOffset = head->nextFree.load(std::memory_order_acquire);
            freeBlockHeader* free = nullptr;

            while (nextFreeOffset != -1) {
                free = reinterpret_cast<freeBlockHeader*>(reinterpret_cast<char*>(this->memoryp) + nextFreeOffset);
                char* blockStart = reinterpret_cast<char*>(free);
                const std::size_t requestSize = lockFreeObject ? allocation_size(blockStart, sizeof(std::atomic<T>)) : allocation_size(blockStart, sizeof(std::atomic<T>));

                if (free->size.load(std::memory_order_acquire) >= requestSize) {
                    free = reinterpret_cast<freeBlockHeader*>(fragment(free, requestSize));

                    std::size_t allocSize = free->size.load(std::memory_order_acquire);
                    blockHeader* header = reinterpret_cast<blockHeader*>(blockStart);
                    header->sizeAndFlags.store(pack_block_header(allocSize, !lockFreeObject), std::memory_order_release);

                    char* objPtr = blockStart + sizeof(blockHeader);
                    objPtr += align8_offset(objPtr);

                    if (lockFreeObject) {
                        // Store lock-free objects as atomics for direct load/store.
                        T initial(std::forward<Args>(args)...);
                        ::new (objPtr) std::atomic<T>(initial);
                    } else {
                        // Shared heap mutex protects non-lock-free construction.
                        pthread_mutex_lock(&head->heapMutex);
                        if (errno == EOWNERDEAD) {
                            pthread_mutex_consistent(&head->heapMutex);
                        }
                        ::new (objPtr) T(std::forward<Args>(args)...);
                        pthread_mutex_unlock(&head->heapMutex);
                    }

                    return static_cast<std::size_t>(objPtr - reinterpret_cast<char*>(this->memoryp));
                }

                nextFreeOffset = free->next.load(std::memory_order_acquire);
            }

            std::size_t currentBrk = head->brk.load(std::memory_order_acquire);
            char* base = reinterpret_cast<char*>(this->memoryp);
            char* blockStart = base + currentBrk;
            const std::size_t requestSize = lockFreeObject ? allocation_size(blockStart, sizeof(std::atomic<T>)) : allocation_size(blockStart, sizeof(T));
            const std::size_t required = currentBrk + requestSize;
            const std::size_t doubled = currentBrk * 2u;
            std::size_t target = required > doubled ? required : doubled;
            if (target == 0 || brk(target) == 0) {
                return 0;
            }

            if (verrifyMapping() < 0) {
                return 0;
            }
        }

        return 0;
    }

    template<typename T>
    void write_at(std::size_t offset, const T& value) {
        if (verrifyMapping() < 0) {
            return;
        }

        char* base = reinterpret_cast<char*>(this->memoryp);
        char* objPtr = base + offset;

        char* blockStart = align8_down(objPtr - sizeof(blockHeader));
        auto* header = reinterpret_cast<blockHeader*>(blockStart);
        const bool needLock = unpack_need_lock(header->sizeAndFlags.load(std::memory_order_acquire));

        if (needLock) {
            heapHeader* head = reinterpret_cast<heapHeader*>(this->memoryp);
            pthread_mutex_lock(&head->heapMutex);
            if (errno == EOWNERDEAD) {
                pthread_mutex_consistent(&head->heapMutex);
            }
            *reinterpret_cast<T*>(objPtr) = value;
            pthread_mutex_unlock(&head->heapMutex);
        } else {
            auto* atomicPtr = reinterpret_cast<std::atomic<T>*>(objPtr);
            atomicPtr->store(value, std::memory_order_release);
        }
    }

    template<typename T>
    T read_at(std::size_t offset) {
        if (verrifyMapping() < 0) {
            return T{};
        }

        char* base = reinterpret_cast<char*>(this->memoryp);
        char* objPtr = base + offset;

        char* blockStart = align8_down(objPtr - sizeof(blockHeader));
        auto* header = reinterpret_cast<blockHeader*>(blockStart);
        const bool needLock = unpack_need_lock(header->sizeAndFlags.load(std::memory_order_acquire));

        T value{};
        if (needLock) {
            heapHeader* head = reinterpret_cast<heapHeader*>(this->memoryp);
            pthread_mutex_lock(&head->heapMutex);
            if (errno == EOWNERDEAD) {
                pthread_mutex_consistent(&head->heapMutex);
            }
            value = *reinterpret_cast<T*>(objPtr);
            pthread_mutex_unlock(&head->heapMutex);
        } else {
            auto* atomicPtr = reinterpret_cast<std::atomic<T>*>(objPtr);
            value = atomicPtr->load(std::memory_order_acquire);
        }

        return value;
    }

    void free(std::size_t offset) {
        if (offset == 0) {
            return;
        }

        if (verrifyMapping() < 0) {
            return;
        }

        char* base = reinterpret_cast<char*>(this->memoryp);
        char* objPtr = base + offset;

        char* blockStart = align8_down(objPtr - sizeof(blockHeader));

        if (align8_offset(blockStart) != 0) {
            std::cerr << "free unaligned";
            return;
        }

        auto* header = reinterpret_cast<blockHeader*>(blockStart);
        std::size_t blockSize = unpack_size(header->sizeAndFlags.load(std::memory_order_acquire));

        auto* block = reinterpret_cast<freeBlockHeader*>(blockStart);
        heapHeader* head = reinterpret_cast<heapHeader*>(this->memoryp);
        block->size.store(blockSize, std::memory_order_release);

        auto unlink_block = [&](freeBlockHeader* target, char* targetStart) {
            const std::int64_t prevOffset = target->prev.load(std::memory_order_acquire);
            const std::int64_t nextOffset = target->next.load(std::memory_order_acquire);

            if (prevOffset != -1) {
                auto* prev = reinterpret_cast<freeBlockHeader*>(base + prevOffset);
                prev->next.store(nextOffset, std::memory_order_release);
            } else {
                head->nextFree.store(nextOffset, std::memory_order_release);
            }

            if (nextOffset != -1) {
                auto* next = reinterpret_cast<freeBlockHeader*>(base + nextOffset);
                next->prev.store(prevOffset, std::memory_order_release);
            }

            target->prev.store(-1, std::memory_order_release);
            target->next.store(-1, std::memory_order_release);
        };

        // Coalesce by scanning the free list for adjacent blocks.
        std::int64_t curOffset = head->nextFree.load(std::memory_order_acquire);
        freeBlockHeader* left = nullptr;
        freeBlockHeader* right = nullptr;
        char* leftStart = nullptr;
        char* rightStart = nullptr;

        while (curOffset != -1) {
            char* curStart = base + curOffset;
            auto* cur = reinterpret_cast<freeBlockHeader*>(curStart);
            const std::size_t curSize = cur->size.load(std::memory_order_acquire);

            if (curStart + curSize == blockStart) {
                left = cur;
                leftStart = curStart;
            } else if (blockStart + blockSize == curStart) {
                right = cur;
                rightStart = curStart;
            }

            if (left && right) {
                break;
            }
            curOffset = cur->next.load(std::memory_order_acquire);
        }

        if (left) {
            unlink_block(left, leftStart);
            blockStart = leftStart;
            block = left;
            blockSize += left->size.load(std::memory_order_acquire);
        }

        if (right) {
            unlink_block(right, rightStart);
            blockSize += right->size.load(std::memory_order_acquire);
        }

        block->size.store(blockSize, std::memory_order_release);

        const std::int64_t blockOffset = static_cast<std::int64_t>(blockStart - base);
        const std::int64_t oldHead = head->nextFree.load(std::memory_order_acquire);

        block->prev.store(-1, std::memory_order_release);
        block->next.store(oldHead, std::memory_order_release);

        if (oldHead != -1) {
            auto* oldHeadPtr = reinterpret_cast<freeBlockHeader*>(base + oldHead);
            oldHeadPtr->prev.store(blockOffset, std::memory_order_release);
        }

        head->nextFree.store(blockOffset, std::memory_order_release);
    }

private:
    int heapfd;
    void* memoryp;

    std::string name;
    bool isOwner;
    std::uint64_t localVersion;
    std::uint64_t localBrk;

    int verrifyMapping() {
        std::uint64_t totalWait = 0;

        std::uint64_t currentVersion = reinterpret_cast<heapHeader*>(this->memoryp)->version.load(std::memory_order_acquire);
        while (currentVersion % 2 != 0) {
            usleep(1000);
            totalWait += 1000;

            if (totalWait >= 50000) {
                return -1;
            }

            currentVersion = reinterpret_cast<heapHeader*>(this->memoryp)->version.load(std::memory_order_acquire);
        }

        if (currentVersion != this->localVersion) {
            std::size_t newBrk = reinterpret_cast<heapHeader*>(this->memoryp)->brk.load(std::memory_order_acquire);
            if (munmap(this->memoryp, this->localBrk) < 0) {
                std::cerr << "brk munmap";
                return -1;
            }

            this->memoryp = mmap(NULL, newBrk, PROT_READ | PROT_WRITE, MAP_SHARED, this->heapfd, 0);
            if (this->memoryp == MAP_FAILED) {
                std::cerr <<"brk mmap";
                return -1;
            }

            this->localVersion = currentVersion;
            this->localBrk = newBrk;
        }

        return 0;
    }

    bool atomics_lock_free() const {
        return std::atomic<std::uint8_t>().is_lock_free() &&
            std::atomic<pid_t>().is_lock_free() &&
            std::atomic<std::uint64_t>().is_lock_free() &&
            std::atomic<std::int64_t>().is_lock_free() &&
            std::atomic<std::size_t>().is_lock_free();
    }

    std::size_t brk(std::size_t bytes) {
        if (verrifyMapping() < 0) {
            return 0;
        }

        heapHeader* head = reinterpret_cast<heapHeader*>(this->memoryp);
        if (head->noResize.load(std::memory_order_acquire) != 0) {
            return 0;
        }

        std::size_t currentBrk = head->brk.load(std::memory_order_acquire);
        if (bytes <= currentBrk) {
            return currentBrk;
        }

        bytes = (bytes + 7u) & ~static_cast<std::size_t>(7u);
        if (bytes <= currentBrk) {
            return currentBrk;
        }

        std::uint64_t version = head->version.load(std::memory_order_acquire);
        std::uint64_t totalWait = 0;
        while (true) {
            if ((version % 2) != 0) {
                usleep(1000);
                totalWait += 1000;
                if (totalWait >= 50000) {
                    return 0;
                }
                version = head->version.load(std::memory_order_acquire);
                continue;
            }

            std::uint64_t desired = version + 1;
            if (head->version.compare_exchange_weak(version, desired, std::memory_order_acq_rel)) {
                version = desired;
                break;
            }
        }

        if (ftruncate(this->heapfd, bytes) < 0) {
            head->version.store(version + 1, std::memory_order_release);
            return 0;
        }

        if (munmap(this->memoryp, this->localBrk) < 0) {
            head->version.store(version + 1, std::memory_order_release);
            return 0;
        }

        this->memoryp = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, this->heapfd, 0);
        if (this->memoryp == MAP_FAILED) {
            head->version.store(version + 1, std::memory_order_release);
            return 0;
        }

        head = reinterpret_cast<heapHeader*>(this->memoryp);
        char* base = reinterpret_cast<char*>(this->memoryp);

        std::size_t oldBrk = currentBrk;
        std::size_t newBlockSize = bytes - oldBrk;

        freeBlockHeader* block = reinterpret_cast<freeBlockHeader*>(base + oldBrk);
        block->size.store(newBlockSize, std::memory_order_release);
        block->prev.store(-1, std::memory_order_release);
        block->next.store(-1, std::memory_order_release);

        auto unlink_block = [&](freeBlockHeader* target) {
            const std::int64_t prevOffset = target->prev.load(std::memory_order_acquire);
            const std::int64_t nextOffset = target->next.load(std::memory_order_acquire);

            if (prevOffset != -1) {
                auto* prev = reinterpret_cast<freeBlockHeader*>(base + prevOffset);
                prev->next.store(nextOffset, std::memory_order_release);
            } else {
                head->nextFree.store(nextOffset, std::memory_order_release);
            }

            if (nextOffset != -1) {
                auto* next = reinterpret_cast<freeBlockHeader*>(base + nextOffset);
                next->prev.store(prevOffset, std::memory_order_release);
            }

            target->prev.store(-1, std::memory_order_release);
            target->next.store(-1, std::memory_order_release);
        };

        std::int64_t curOffset = head->nextFree.load(std::memory_order_acquire);
        freeBlockHeader* left = nullptr;
        freeBlockHeader* right = nullptr;
        char* leftStart = nullptr;
        char* rightStart = nullptr;
        char* blockStart = base + oldBrk;
        std::size_t blockSize = newBlockSize;

        while (curOffset != -1) {
            char* curStart = base + curOffset;
            auto* cur = reinterpret_cast<freeBlockHeader*>(curStart);
            const std::size_t curSize = cur->size.load(std::memory_order_acquire);

            if (curStart + curSize == blockStart) {
                left = cur;
                leftStart = curStart;
            } else if (blockStart + blockSize == curStart) {
                right = cur;
                rightStart = curStart;
            }

            if (left && right) {
                break;
            }
            curOffset = cur->next.load(std::memory_order_acquire);
        }

        if (left) {
            unlink_block(left);
            blockStart = leftStart;
            block = left;
            blockSize += left->size.load(std::memory_order_acquire);
        }

        if (right) {
            unlink_block(right);
            blockSize += right->size.load(std::memory_order_acquire);
        }

        block->size.store(blockSize, std::memory_order_release);

        const std::int64_t blockOffset = static_cast<std::int64_t>(blockStart - base);
        const std::int64_t oldHead = head->nextFree.load(std::memory_order_acquire);

        block->prev.store(-1, std::memory_order_release);
        block->next.store(oldHead, std::memory_order_release);

        if (oldHead != -1) {
            auto* oldHeadPtr = reinterpret_cast<freeBlockHeader*>(base + oldHead);
            oldHeadPtr->prev.store(blockOffset, std::memory_order_release);
        }

        head->nextFree.store(blockOffset, std::memory_order_release);
        head->brk.store(bytes, std::memory_order_release);

        std::uint64_t newVersion = version + 1;
        head->version.store(newVersion, std::memory_order_release);

        this->localBrk = bytes;
        this->localVersion = newVersion;

        return bytes;
    }

    std::size_t allocation_size(char* blockStart, std::size_t objSize) const {
        std::size_t offset = sizeof(blockHeader);
        // Keep payload 8-byte aligned for shared memory safety.
        offset += align8_offset(blockStart + offset);
        offset += objSize;
        offset += align8_offset(blockStart + offset);
        if (offset < sizeof(freeBlockHeader)) {
            offset = sizeof(freeBlockHeader);
        }
        return offset;
    }

    static constexpr std::uint64_t kBlockFlagNeedLock = 0x1ULL;

    static std::uint64_t pack_block_header(std::size_t size, bool needLock) {
        std::uint64_t value = static_cast<std::uint64_t>(size);
        if (needLock) {
            value |= kBlockFlagNeedLock;
        }
        return value;
    }

    static std::size_t unpack_size(std::uint64_t value) {
        return static_cast<std::size_t>(value & ~kBlockFlagNeedLock);
    }

    static bool unpack_need_lock(std::uint64_t value) {
        return (value & kBlockFlagNeedLock) != 0;
    }

    void* fragment(void* ptr, std::size_t size) {
        freeBlockHeader* block = reinterpret_cast<freeBlockHeader*>(ptr);
        const std::size_t blockSize = block->size.load(std::memory_order_acquire);
        char* blockStart = reinterpret_cast<char*>(block);
        const std::size_t alignedSize = size + align8_offset(blockStart + size);
        heapHeader* head = reinterpret_cast<heapHeader*>(this->memoryp);

        std::int64_t prevOffset = block->prev.load(std::memory_order_acquire);
        std::int64_t nextOffset = block->next.load(std::memory_order_acquire);

        if (blockSize > alignedSize + sizeof(freeBlockHeader) + 8u) {
            char* newFreeStart = blockStart + alignedSize;
            freeBlockHeader* newFree = reinterpret_cast<freeBlockHeader*>(newFreeStart);
            const std::size_t remainingSize = blockSize - alignedSize;

            newFree->size.store(remainingSize, std::memory_order_release);
            newFree->prev.store(prevOffset, std::memory_order_release);
            newFree->next.store(nextOffset, std::memory_order_release);

            if (prevOffset != -1) {
                auto* prev = reinterpret_cast<freeBlockHeader*>(reinterpret_cast<char*>(this->memoryp) + prevOffset);
                prev->next.store(static_cast<std::int64_t>(newFreeStart - reinterpret_cast<char*>(this->memoryp)), std::memory_order_release);
            } else {
                head->nextFree.store(static_cast<std::int64_t>(newFreeStart - reinterpret_cast<char*>(this->memoryp)), std::memory_order_release);
            }

            if (nextOffset != -1) {
                auto* next = reinterpret_cast<freeBlockHeader*>(reinterpret_cast<char*>(this->memoryp) + nextOffset);
                next->prev.store(static_cast<std::int64_t>(newFreeStart - reinterpret_cast<char*>(this->memoryp)), std::memory_order_release);
            }

            block->size.store(alignedSize, std::memory_order_release);
            return block;
        }

        if (prevOffset != -1) {
            auto* prev = reinterpret_cast<freeBlockHeader*>(reinterpret_cast<char*>(this->memoryp) + prevOffset);
            prev->next.store(nextOffset, std::memory_order_release);
        } else {
            head->nextFree.store(nextOffset, std::memory_order_release);
        }

        if (nextOffset != -1) {
            auto* next = reinterpret_cast<freeBlockHeader*>(reinterpret_cast<char*>(this->memoryp) + nextOffset);
            next->prev.store(prevOffset, std::memory_order_release);
        }

        block->size.store(blockSize, std::memory_order_release);
        return block;
    }
};