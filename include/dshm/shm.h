#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>

namespace dshm {

static constexpr std::uint8_t K_SHM_LINK = 1U << 0;

static constexpr std::uint8_t K_STAT_OWNER = 1U << 0;
static constexpr std::uint8_t K_STAT_CREATOR = 1U << 1;

static std::size_t round8_down(std::size_t n) {
    return n & ~7;
}

static std::size_t round8_up(std::size_t n) {
    return (n + 7u) & ~static_cast<std::size_t>(7u);
}

typedef struct shm_stat {
    std::size_t size;
    std::uint64_t meta;
} shm_stat;

inline int shm_stat_fd(shm_stat* sstat) {
    if (!sstat) {
        return -1;
    }

    return static_cast<int>(sstat->meta >> 32); 
}

inline bool shm_stat_owner(shm_stat* sstat) {
    return (sstat->meta & K_STAT_OWNER) != 0;
}

inline bool shm_stat_creator(shm_stat* sstat) {
    return (sstat->meta & K_STAT_CREATOR) != 0;
}

typedef struct shm_header {
    std::atomic<std::size_t> brk;
    std::atomic<std::uint64_t> version;
    std::atomic<pid_t> ownerPID;
    std::atomic<std::uint8_t> meta;
} shm_header;

// meta: | 0 | 0 | 0 | LINK BIT |

inline void* create_shm(std::string name, shm_stat* sstat) {
    if (!sstat) {
        return nullptr;
    }

    const static std::size_t pageSize = sysconf(_SC_PAGESIZE);
    const static std::size_t defaultSize = 2 * pageSize;
    
    std::uint8_t owner = 0U;
    std::uint8_t creator = 0U;
    std::uint8_t version = 0;

    std::uint8_t stat_meta = 0U;
    std::uint8_t shm_meta = 0U;

    bool newSHM = false;
    std::size_t bufferSize = 0;

    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
    if (fd != -1) {
        newSHM = true;
    } else if(fd == -1 && errno == EEXIST) {
        fd = shm_open(name.c_str(), O_CREAT | O_RDWR, S_IRWXU | S_IRWXG | S_IRWXO);
        if (fd == -1) {
            return nullptr;
        }

        struct stat st;
        if(fstat(fd, &st) < 0) {
            return nullptr;
        }

        bufferSize = st.st_size;
        if (bufferSize < defaultSize) {
            newSHM = true;
        }
    } else {
        return nullptr;
    }

    if (newSHM) {
        owner = K_STAT_OWNER;
        creator = K_STAT_CREATOR;

        if (ftruncate(fd, defaultSize) < 0) {
            return nullptr;
        }
        bufferSize = defaultSize;

        shm_header* head = reinterpret_cast<shm_header*>(mmap(NULL, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (head == MAP_FAILED) {
            return nullptr;
        }

        head->ownerPID.store(getpid(), std::memory_order_release);
        head->brk.store(defaultSize - sizeof(shm_header), std::memory_order_release);
        head->version.store(0, std::memory_order_release);

        shm_meta |= K_SHM_LINK;
        head->meta.store(shm_meta, std::memory_order_release);
    } else {
        shm_header* head = reinterpret_cast<shm_header*>(mmap(NULL, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (head == MAP_FAILED) {
            return nullptr;
        }

        std::size_t ownerPID = head->ownerPID.load(std::memory_order_acquire);
        bool ownerRunning = false;
        if (kill(ownerPID, 0) == 0) {
            ownerRunning = true;
        } else if (errno == EPERM) {
            ownerRunning = true;
        }

        if (!ownerRunning) {
            head->ownerPID.store(getpid(), std::memory_order_release);
        }
    }

    stat_meta = static_cast<std::uint64_t>(fd) << 32;
    stat_meta |= owner;
    stat_meta |= creator;

    sstat->size = bufferSize - sizeof(shm_header);
    sstat->meta = stat_meta;


    void* ptr = mmap(NULL, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }

    ptr = reinterpret_cast<void*>(reinterpret_cast<char*>(ptr) + sizeof(shm_header));

    return ptr;
}

inline std::size_t remap_shm(void** ptr, shm_stat* sstat) {
    if (!sstat) {
        return 0;
    }

    if (!ptr || !*ptr) {
        return 0;
    }

    void* buffer = reinterpret_cast<void*>(reinterpret_cast<char*>(*ptr) - sizeof(shm_header));
    shm_header* head = reinterpret_cast<shm_header*>(buffer);
    std::size_t meta = head->meta.load(std::memory_order_acquire);
    if (meta & K_SHM_LINK) {
        return 0;
    }
    std::size_t bufferSize = head->brk.load(std::memory_order_acquire) + sizeof(shm_header);

    if (munmap(buffer, sstat->size) < 0) {
        return 0;
    }
    buffer = nullptr;


    buffer = mmap(NULL, bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, shm_stat_fd(sstat), 0);
    if (buffer == MAP_FAILED) {
        return 0;
    }

    *ptr = buffer;
    return bufferSize;
}

inline bool verify_shm(void** ptr, shm_stat* sstat) {
    if (!sstat) {
        return false;
    }

    if (!ptr || !*ptr) {
        return false;
    }

    void* buffer = reinterpret_cast<void*>(reinterpret_cast<char*>(*ptr) - sizeof(shm_header));
    shm_header* head = reinterpret_cast<shm_header*>(buffer);
    std::uint64_t totalWait = 0;

    std::uint64_t version = head->version.load(std::memory_order_acquire);
    while (version % 2 != 0) {
        usleep(1000);
        totalWait += 1000;

        if (totalWait >= 50000) {
            return false;
        }
        version = head->version.load(std::memory_order_acquire);
    }

    if (sstat->size != head->brk.load(std::memory_order_acquire)) {
        if (remap_shm(ptr, sstat) == 0) {
            return false;
        }
    }
    return true;
}

inline bool unmap_shm(void** ptr, shm_stat* sstat) {
    if (!verify_shm(ptr, sstat)) {
        return false;
    }

    void* buffer = reinterpret_cast<void*>(reinterpret_cast<char*>(*ptr) - sizeof(shm_header));
    shm_header* head = reinterpret_cast<shm_header*>(buffer);

    std::size_t bufferSize = head->brk.load(std::memory_order_acquire) + sizeof(shm_header);
    if(munmap(buffer, bufferSize) < 0) {
        return false;
    }
    *ptr = nullptr;
    return true;
}

inline bool unlink_shm(void** ptr, std::string name, shm_stat* sstat) {
    if (!verify_shm(ptr, sstat)) {
        return false;
    }

    void* buffer = reinterpret_cast<void*>(reinterpret_cast<char*>(*ptr) - sizeof(shm_header));
    shm_header* head = reinterpret_cast<shm_header*>(buffer);
    std::uint8_t meta = head->meta.load(std::memory_order_acquire);
    meta |= K_SHM_LINK;
    head->meta.store(meta, std::memory_order_release);

    shm_unlink(name.c_str());

    int fd = shm_stat_fd(sstat);
    close(fd);

    return true;
}

inline std::size_t resize_shm(void** ptr, shm_stat* sstat, std::size_t size) {
    if (!verify_shm(ptr, sstat)) {
        return 0;
    }

    size = round8_up(size);

    void* buffer = reinterpret_cast<void*>(reinterpret_cast<char*>(*ptr) - sizeof(shm_header));
    shm_header* head = reinterpret_cast<shm_header*>(buffer);
    std::size_t meta = head->meta.load(std::memory_order_acquire);
    if (meta & K_SHM_LINK) {
        return 0;
    }

    std::size_t bufferSize = head->brk.load(std::memory_order_acquire) + sizeof(shm_header);
    if (size + sizeof(shm_header) < bufferSize) {
        return 0;
    } else if(munmap(buffer, bufferSize) < 0) {
        return 0;
    }
    buffer = nullptr;

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
        
        std::uint64_t desierdVersion = version + 1;
        if (head->version.compare_exchange_weak(version, desierdVersion, std::memory_order_acq_rel)) {
            version = desierdVersion;
            break;
        }
    }

    if (ftruncate(shm_stat_fd(sstat), size) < 0) {
        head->version.store(version + 1, std::memory_order_release);
        return 0;
    }

    buffer = mmap(NULL, size + sizeof(shm_header), PROT_READ | PROT_WRITE, MAP_SHARED, shm_stat_fd(sstat), 0);
    if (buffer == MAP_FAILED) {
        return 0;
    }

    *ptr = reinterpret_cast<void*>(reinterpret_cast<char*>(buffer) + sizeof(shm_header));
    sstat->size = size;
    return size;
}

}