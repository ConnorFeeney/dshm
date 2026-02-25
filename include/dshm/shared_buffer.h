#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>

#include "dshm/flags.h"

#ifdef __linux__

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h> 
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>

namespace dshm {
    DECLARE_FLAGS(BufferFlags);
    DEFINE_FLAGS(BufferFlags, OWN, 0);
    DEFINE_FLAGS(BufferFlags, OWNF, 1);
    DEFINE_FLAGS(BufferFlags, READ, 2);
    DEFINE_FLAGS(BufferFlags, WRITE, 3);

    class buffer_error_category : public std::error_category {
    public:
        static const std::error_category& get() noexcept {
            static buffer_error_category instance;
            return instance;
        }

        virtual const char* name() const noexcept override {
            return "BufferError";
        }

        virtual std::string message(int ev) const override {
            return "";
        }
    };

    enum buffer_error_code : int {
        INVLAID_FLAGS = 1 << 0,
        OPEN_BUFFER_FAILLED = 1 << 1,
        INVALID_PERMISSIONS = 1 << 2,
        STAT_FAILED = 1 << 3,
        TRUNCATE_FAILED = 1 << 4,
        MMAP_FAILED = 1 << 5,
        INVALID_SIZE = 1 << 6,
        OWNERSHIP_EXISTS = 1 << 7,
        OWNERSHIP_TAKE_FAILED = 1 << 8
    };

    typedef struct buffer_header {
        pthread_mutex_t mutex;
        std::atomic<pid_t> ownerPid;
        std::atomic<std::uint64_t> bufSize;
        std::atomic<std::uint64_t> version;
        std::atomic<std::uint8_t> remapRequired;
        std::uint8_t reserved[7];
    } buffer_header;

    class SharedBuffer {
    public:
        SharedBuffer(const char* name, BufferFlags flags, std::error_code& ec)
            : buffer_fd(-1), p(nullptr), flags(flags), isOwner(false), name(name ? name : ""), localVersion(0) {
            ec.clear();

            int accessFlags = 0;
            mode_t mode = 0;

            if (flags & OWN && flags & OWNF) {
                ec = std::error_code(buffer_error_code::INVLAID_FLAGS, buffer_error_category::get());
                return;
            }

            if (!(flags & READ)) {
                ec = std::error_code(buffer_error_code::INVLAID_FLAGS, buffer_error_category::get());
                return;
            }

            if ((flags & OWN || flags & OWNF) && !(flags & WRITE)) {
                ec = std::error_code(buffer_error_code::INVLAID_FLAGS, buffer_error_category::get());
                return;
            }

            if (flags & READ && flags & WRITE) {
                accessFlags |= O_RDWR;
            } else {
                accessFlags |= O_RDONLY;
            }

            if (flags & OWN || flags & OWNF) {
                mode |= S_IROTH | S_IRUSR | S_IRGRP;
                if (flags & WRITE) {
                    mode |= S_IWOTH | S_IWUSR | S_IWGRP;
                }
            }

            bool ownershipRequested = (flags & OWN) || (flags & OWNF);
            bool forceOwnership = (flags & OWNF);
            bool createdNew = false;

            if (ownershipRequested) {
                this->buffer_fd = shm_open(name, accessFlags | O_CREAT | O_EXCL, mode);
                if (this->buffer_fd >= 0) {
                    createdNew = true;
                } else if (errno == EEXIST) {
                    this->buffer_fd = shm_open(name, accessFlags, 0);
                }
            } else {
                this->buffer_fd = shm_open(name, accessFlags, 0);
            }

            if (this->buffer_fd < 0) {
                ec = std::error_code(buffer_error_code::OPEN_BUFFER_FAILLED, buffer_error_category::get());
                return;
            }

            struct stat st;
            if (fstat(this->buffer_fd, &st) != 0) {
                ec = std::error_code(buffer_error_code::STAT_FAILED, buffer_error_category::get());
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return;
            }
            
            mode_t perms = st.st_mode & 0777;
            mode_t requiredPerms = S_IROTH;
            if ((perms & requiredPerms) != requiredPerms) {
                ec = std::error_code(buffer_error_code::INVALID_PERMISSIONS, buffer_error_category::get());
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return;
            }

            long pageSizeRaw = sysconf(_SC_PAGE_SIZE);
            if (pageSizeRaw <= 0) {
                ec = std::error_code(buffer_error_code::STAT_FAILED, buffer_error_category::get());
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return;
            }
            std::size_t pageSize = static_cast<std::size_t>(pageSizeRaw);

            if (st.st_size > 0 && st.st_size < static_cast<off_t>(sizeof(buffer_header))) {
                ec = std::error_code(buffer_error_code::INVALID_SIZE, buffer_error_category::get());
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return;
            }

            bool bufferExists = st.st_size >= static_cast<off_t>(sizeof(buffer_header));
            bool takeOwnership = false;
            std::size_t existingSize = st.st_size > 0 ? static_cast<std::size_t>(st.st_size) : 0;

            buffer_header* headerView = nullptr;
            if (!createdNew && bufferExists) {
                int headerProt = ownershipRequested ? (PROT_READ | PROT_WRITE) : PROT_READ;
                headerView = reinterpret_cast<buffer_header*>(
                    mmap(NULL, sizeof(buffer_header), headerProt, MAP_SHARED, this->buffer_fd, 0));
                if (headerView == MAP_FAILED) {
                    ec = std::error_code(buffer_error_code::MMAP_FAILED, buffer_error_category::get());
                    close(this->buffer_fd);
                    this->buffer_fd = -1;
                    return;
                }

                std::uint64_t headerSize = headerView->bufSize.load(std::memory_order_acquire);
                if (headerSize > 0) {
                    existingSize = static_cast<std::size_t>(headerSize);
                }
            }

            if (ownershipRequested) {
                if (createdNew) {
                    takeOwnership = true;
                }
                bool ownerAlive = false;
                if (!createdNew && bufferExists) {
                    pid_t ownerPid = headerView->ownerPid.load(std::memory_order_acquire);
                    if (ownerPid > 0) {
                        if (kill(ownerPid, 0) == 0) {
                            ownerAlive = true;
                        } else if (errno != ESRCH) {
                            ownerAlive = true;
                        }
                    }
                }

                if (!createdNew && bufferExists && ownerAlive) {
                    if (forceOwnership) {
                        ec = std::error_code(buffer_error_code::OWNERSHIP_EXISTS, buffer_error_category::get());
                        munmap(headerView, sizeof(buffer_header));
                        close(this->buffer_fd);
                        this->buffer_fd = -1;
                        return;
                    }
                } else if (!createdNew) {
                    takeOwnership = true;
                }
            }

            if (takeOwnership) {
                if (!createdNew) {
                    if (shm_unlink(name) != 0) {
                        ec = std::error_code(buffer_error_code::OWNERSHIP_TAKE_FAILED, buffer_error_category::get());
                        return;
                    }

                    if (bufferExists && headerView != nullptr) {
                        headerView->remapRequired.store(1, std::memory_order_release);
                        msync(headerView, sizeof(buffer_header), MS_SYNC);
                        munmap(headerView, sizeof(buffer_header));
                        headerView = nullptr;
                    }

                    close(this->buffer_fd);
                    this->buffer_fd = -1;

                    int createFlags = accessFlags | O_CREAT | O_EXCL;
                    this->buffer_fd = shm_open(name, createFlags, mode);
                    if (this->buffer_fd < 0) {
                        ec = std::error_code(buffer_error_code::OPEN_BUFFER_FAILLED, buffer_error_category::get());
                        return;
                    }
                }

                std::size_t newSize = existingSize > 0 ? existingSize : (2 * pageSize);
                if (ftruncate(this->buffer_fd, static_cast<off_t>(newSize)) != 0) {
                    ec = std::error_code(buffer_error_code::TRUNCATE_FAILED, buffer_error_category::get());
                    close(this->buffer_fd);
                    this->buffer_fd = -1;
                    return;
                }

                buffer_header* newHeader = reinterpret_cast<buffer_header*>(
                    mmap(NULL, sizeof(buffer_header), PROT_READ | PROT_WRITE, MAP_SHARED, this->buffer_fd, 0));
                if (newHeader == MAP_FAILED) {
                    ec = std::error_code(buffer_error_code::MMAP_FAILED, buffer_error_category::get());
                    close(this->buffer_fd);
                    this->buffer_fd = -1;
                    return;
                }
                pthread_mutexattr_t mutexAttr;
                if (pthread_mutexattr_init(&mutexAttr) != 0 ||
                    pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED) != 0 ||
                    pthread_mutexattr_setrobust(&mutexAttr, PTHREAD_MUTEX_ROBUST) != 0 ||
                    pthread_mutex_init(&newHeader->mutex, &mutexAttr) != 0) {
                    pthread_mutexattr_destroy(&mutexAttr);
                    munmap(newHeader, sizeof(buffer_header));
                    ec = std::error_code(buffer_error_code::MMAP_FAILED, buffer_error_category::get());
                    close(this->buffer_fd);
                    this->buffer_fd = -1;
                    return;
                }
                pthread_mutexattr_destroy(&mutexAttr);
                newHeader->ownerPid.store(getpid(), std::memory_order_release);
                newHeader->bufSize.store(static_cast<std::uint64_t>(newSize), std::memory_order_release);
                newHeader->version.store(0, std::memory_order_release);
                newHeader->remapRequired.store(0, std::memory_order_release);
                msync(newHeader, sizeof(buffer_header), MS_SYNC);
                munmap(newHeader, sizeof(buffer_header));
            } else {
                if (headerView != nullptr) {
                    munmap(headerView, sizeof(buffer_header));
                }
            }

            std::size_t mapSize = existingSize;
            if (takeOwnership) {
                mapSize = existingSize > 0 ? existingSize : (2 * pageSize);
            }

            if (mapSize < sizeof(buffer_header)) {
                ec = std::error_code(buffer_error_code::INVALID_SIZE, buffer_error_category::get());
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return;
            }

            int prot = PROT_READ;
            if (flags & WRITE) {
                prot |= PROT_WRITE;
            }

            this->p = reinterpret_cast<char*>(mmap(NULL, mapSize, prot, MAP_SHARED, this->buffer_fd, 0));
            if (this->p == MAP_FAILED) {
                this->p = nullptr;
                ec = std::error_code(buffer_error_code::MMAP_FAILED, buffer_error_category::get());
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return;
            }

            this->isOwner = takeOwnership;
            this->localVersion = this->currentVersion();
        }

        ~SharedBuffer() {
            std::size_t mappedSize = this->mappedSize();
            if (this->p != nullptr && mappedSize > 0) {
                munmap(this->p, mappedSize);
            }
            if (this->buffer_fd >= 0) {
                close(this->buffer_fd);
            }
            if (this->isOwner && !this->name.empty()) {
                shm_unlink(this->name.c_str());
            }
        }

        std::size_t write(const char* buffer, std::size_t bytes, std::size_t offset) {
            if (!this->p || !(this->flags & WRITE) || !buffer) {
                return 0;
            }

            if (!this->ensureMapped()) {
                return 0;
            }

            for (;;) {
                if (!this->lockShared()) {
                    return 0;
                }

                buffer_header* header = reinterpret_cast<buffer_header*>(this->p);
                std::uint64_t current = header->version.load(std::memory_order_acquire);
                if ((current & 1U) != 0U) {
                    if (this->recoverIfOwnerDead(header, current)) {
                        this->unlockShared();
                        if (!this->ensureMapped()) {
                            return 0;
                        }
                        continue;
                    }
                    this->unlockShared();
                    usleep(1000);
                    continue;
                }

                if (header->remapRequired.load(std::memory_order_acquire) != 0 ||
                    current != this->localVersion) {
                    this->unlockShared();
                    if (!this->ensureMapped()) {
                        return 0;
                    }
                    continue;
                }

                std::size_t headerSize = sizeof(buffer_header);
                std::size_t mappedSize = this->mappedSize();
                if (mappedSize <= headerSize) {
                    this->unlockShared();
                    return 0;
                }

                std::size_t payloadSize = mappedSize - headerSize;
                if (offset >= payloadSize) {
                    this->unlockShared();
                    return 0;
                }

                std::size_t writable = payloadSize - offset;
                std::size_t toCopy = bytes < writable ? bytes : writable;
                std::memcpy(this->p + headerSize + offset, buffer, toCopy);
                this->unlockShared();
                return toCopy;
            }
        }

        const char* read(std::size_t bytes, std::size_t offset) {
            if (!this->p || !(this->flags & READ)) {
                return nullptr;
            }

            if (!this->ensureMapped()) {
                return nullptr;
            }

            for (;;) {
                if (!this->lockShared()) {
                    return nullptr;
                }

                buffer_header* header = reinterpret_cast<buffer_header*>(this->p);
                std::uint64_t current = header->version.load(std::memory_order_acquire);
                if ((current & 1U) != 0U) {
                    if (this->recoverIfOwnerDead(header, current)) {
                        this->unlockShared();
                        if (!this->ensureMapped()) {
                            return nullptr;
                        }
                        continue;
                    }
                    this->unlockShared();
                    usleep(1000);
                    continue;
                }

                if (header->remapRequired.load(std::memory_order_acquire) != 0 ||
                    current != this->localVersion) {
                    this->unlockShared();
                    if (!this->ensureMapped()) {
                        return nullptr;
                    }
                    continue;
                }

                std::size_t headerSize = sizeof(buffer_header);
                std::size_t mappedSize = this->mappedSize();
                if (mappedSize <= headerSize) {
                    this->unlockShared();
                    return nullptr;
                }

                std::size_t payloadSize = mappedSize - headerSize;
                if (offset >= payloadSize || bytes > payloadSize - offset) {
                    this->unlockShared();
                    return nullptr;
                }

                const char* result = this->p + headerSize + offset;
                this->unlockShared();
                return result;
            }
        }

        std::size_t resize(std::size_t newSize, std::error_code& ec) {
            if (newSize < sizeof(buffer_header)) {
                ec = std::error_code(buffer_error_code::INVALID_SIZE, buffer_error_category::get());
                return 0;
            }

            if (!this->p || !(this->flags & WRITE) || !this->isOwner) {
                ec = std::error_code(buffer_error_code::INVALID_PERMISSIONS, buffer_error_category::get());
                return 0;
            }

            if (!this->lockShared()) {
                ec = std::error_code(buffer_error_code::MMAP_FAILED, buffer_error_category::get());
                return 0;
            }

            buffer_header* header = reinterpret_cast<buffer_header*>(this->p);
            std::uint64_t currVersion = header->version.load(std::memory_order_acquire);
            while ((currVersion & 1U) != 0U) {
                if (this->recoverIfOwnerDead(header, currVersion)) {
                    this->unlockShared();
                    usleep(1000);
                    if (!this->lockShared()) {
                        ec = std::error_code(buffer_error_code::MMAP_FAILED, buffer_error_category::get());
                        return 0;
                    }
                    header = reinterpret_cast<buffer_header*>(this->p);
                    currVersion = header->version.load(std::memory_order_acquire);
                    continue;
                }
                this->unlockShared();
                usleep(1000);
                if (!this->lockShared()) {
                    ec = std::error_code(buffer_error_code::MMAP_FAILED, buffer_error_category::get());
                    return 0;
                }
                header = reinterpret_cast<buffer_header*>(this->p);
                currVersion = header->version.load(std::memory_order_acquire);
            }

            header->version.store(currVersion + 1, std::memory_order_release);
            header->remapRequired.store(1, std::memory_order_release);

            if (ftruncate(this->buffer_fd, static_cast<off_t>(newSize)) != 0) {
                ec = std::error_code(buffer_error_code::TRUNCATE_FAILED, buffer_error_category::get());
                header->version.store(currVersion, std::memory_order_release);
                this->unlockShared();
                return 0;
            }

            header->bufSize.store(static_cast<std::uint64_t>(newSize), std::memory_order_release);
            header->version.store(currVersion + 2, std::memory_order_release);
            this->unlockShared();

            this->ensureMapped();

            return newSize;
        }

        std::size_t size() {
            return this->mappedSize();
        }
        
    private:
        bool recoverIfOwnerDead(buffer_header* header, std::uint64_t currentVersion) {
            pid_t ownerPid = header->ownerPid.load(std::memory_order_acquire);
            if (ownerPid <= 0) {
                return false;
            }

            if (kill(ownerPid, 0) == 0 || errno != ESRCH) {
                return false;
            }

            std::uint64_t evenVersion = (currentVersion % 2 == 0) ? currentVersion : currentVersion + 1;
            header->version.store(evenVersion, std::memory_order_release);
            header->remapRequired.store(1, std::memory_order_release);
            return true;
        }

        std::uint64_t currentVersion() const {
            if (!this->p) {
                return 0;
            }
            const buffer_header* header = reinterpret_cast<const buffer_header*>(this->p);
            return header->version.load(std::memory_order_acquire);
        }

        bool lockShared() {
            if (!this->p) {
                return false;
            }
            buffer_header* header = reinterpret_cast<buffer_header*>(this->p);
            int rc = pthread_mutex_lock(&header->mutex);
            if (rc == EOWNERDEAD) {
                pthread_mutex_consistent(&header->mutex);
                return true;
            }
            return rc == 0;
        }

        void unlockShared() {
            if (!this->p) {
                return;
            }
            buffer_header* header = reinterpret_cast<buffer_header*>(this->p);
            pthread_mutex_unlock(&header->mutex);
        }

        bool ensureMapped() {
            if (!this->p) {
                return false;
            }

            buffer_header* header = reinterpret_cast<buffer_header*>(this->p);
            std::uint64_t current = header->version.load(std::memory_order_acquire);
            bool needsRemap = header->remapRequired.load(std::memory_order_acquire) != 0 || current != this->localVersion;
            if (!needsRemap) {
                return true;
            }

            std::size_t oldSize = static_cast<std::size_t>(
                header->bufSize.load(std::memory_order_acquire));
            if (oldSize > 0) {
                munmap(this->p, oldSize);
            }
            this->p = nullptr;

            if (this->buffer_fd >= 0) {
                close(this->buffer_fd);
                this->buffer_fd = -1;
            }

            int accessFlags = (this->flags & READ && this->flags & WRITE) ? O_RDWR : O_RDONLY;
            this->buffer_fd = shm_open(this->name.c_str(), accessFlags, 0);
            if (this->buffer_fd < 0) {
                return false;
            }

            struct stat st;
            if (fstat(this->buffer_fd, &st) != 0) {
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return false;
            }

            std::size_t mapSize = st.st_size > 0 ? static_cast<std::size_t>(st.st_size) : 0;
            if (mapSize < sizeof(buffer_header)) {
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return false;
            }

            int prot = PROT_READ;
            if (this->flags & WRITE) {
                prot |= PROT_WRITE;
            }

            this->p = reinterpret_cast<char*>(mmap(NULL, mapSize, prot, MAP_SHARED, this->buffer_fd, 0));
            if (this->p == MAP_FAILED) {
                this->p = nullptr;
                close(this->buffer_fd);
                this->buffer_fd = -1;
                return false;
            }

            buffer_header* newHeader = reinterpret_cast<buffer_header*>(this->p);
            this->localVersion = newHeader->version.load(std::memory_order_acquire);
            if (this->flags & WRITE) {
                newHeader->remapRequired.store(0, std::memory_order_release);
            }
            return true;
        }

        std::size_t mappedSize() const {
            if (!this->p) {
                return 0;
            }
            const buffer_header* header = reinterpret_cast<const buffer_header*>(this->p);
            return static_cast<std::size_t>(header->bufSize.load(std::memory_order_acquire));
        }

        int buffer_fd;
        char* p;
        
        BufferFlags flags;
        bool isOwner;

        std::uint64_t localVersion;

        std::string name;
    };
}
#endif