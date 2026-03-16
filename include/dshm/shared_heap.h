#pragma once

#include "dshm/shm.h"
#include <unordered_map>


typedef struct heap_header {
    pthread_mutex_t heapMutex;
    pthread_mutex_t blockMutex;
    pthread_mutex_t tableMutex;

    std::atomic<std::size_t> table;
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

typedef struct rb_node {
    std::size_t key_addr;
    std::size_t key_size;
    std::size_t data_addr;

    std::size_t parent;
    std::size_t left;
    std::size_t right;

    bool isRed;
} rb_node;

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
       
        if (shm_stat_creator(&this->sstat)) {
            heap_header* head = reinterpret_cast<heap_header*>(this->memory);

            pthread_mutexattr_t attr;
            if (pthread_mutexattr_init(&attr) != 0) {
                return;
            }
            if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
                pthread_mutexattr_destroy(&attr);
                return;
            }
            if (
                pthread_mutex_init(&head->heapMutex, &attr) != 0 ||
                pthread_mutex_init(&head->blockMutex, &attr) != 0 ||
                pthread_mutex_init(&head->tableMutex, &attr) != 0
            ) {
                pthread_mutexattr_destroy(&attr);
                return;
            }
            pthread_mutexattr_destroy(&attr);

            std::size_t nextf = sizeof(heap_header) + align8_offset(reinterpret_cast<char*>(head) + sizeof(heap_header));
            block_header* free = reinterpret_cast<block_header*>(reinterpret_cast<char*>(this->memory) + nextf);
            free->next.store(-1, std::memory_order_release);
            free->prev.store(-1, std::memory_order_release);
            std::size_t blockSize = round8_down(sstat.size - nextf);
            free->meta.store(pack_block_meta(blockSize, 0), std::memory_order_release);

            head->block.store(nextf, std::memory_order_release);

            rb_node startNode;

            startNode.data_addr = 0;
            startNode.key_addr = 0;
            startNode.key_size = 0;

            startNode.parent = 0;
            startNode.left = 0;
            startNode.right = 0;

            startNode.isRed = false;

            std::size_t table = this->allocate<rb_node>(startNode);
            head->table.store(table, std::memory_order_release);
        }
    }

    ~shared_heap() {
        if (shm_stat_owner(&this->sstat)) {
            unlink_shm(&this->memory, this->name, &this->sstat);
        }

        unmap_shm(&this->memory, &this->sstat);
    }

    template<typename T, typename... Args>
    std::size_t allocate(Args... args) {
        static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
        if (!verify_shm(&this->memory, &sstat)) {
            return 0;
        }
        const bool lockFree = std::atomic<T>::is_always_lock_free;
        const std::size_t objSize = lockFree ? sizeof(std::atomic<T>) : sizeof(T);
        block_header* block = nullptr;

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        pthread_mutex_lock(&head->blockMutex);
        if (errno == EOWNERDEAD) {
            pthread_mutex_consistent(&head->blockMutex);
        }
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
        pthread_mutex_unlock(&head->blockMutex);

        if (lockFree) {
            T initial(std::forward<Args>(args)...);
            ::new (objPtr) std::atomic<T>(initial);
        } else {
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
        static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
        if (!verify_shm(&this->memory, &sstat)) {
            return 0;
        }
        const bool lockFree = std::atomic<T>::is_always_lock_free;
        const std::size_t arraySizeBytes = sizeof(T) * size;
        block_header* block = nullptr;

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        pthread_mutex_lock(&head->blockMutex);
        if (errno == EOWNERDEAD) {
            pthread_mutex_consistent(&head->blockMutex);
        }
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
        pthread_mutex_unlock(&head->blockMutex);

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

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        pthread_mutex_lock(&head->blockMutex);
        if (errno == EOWNERDEAD) {
            pthread_mutex_consistent(&head->blockMutex);
        }
        std::size_t blockMeta = block->meta.load(std::memory_order_acquire);
        std::size_t blockFlags = unpack_block_flags(blockMeta);
        if ((blockFlags & K_BLOCK_ALLOCATED) == 0) {
            pthread_mutex_unlock(&head->blockMutex);
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
        pthread_mutex_unlock(&head->blockMutex);
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
    requires std::is_integral_v<T>
    bool fetch_add(std::size_t addr, T amount) {
        if (!verify_shm(&this->memory, &sstat)) {
            return false;
        }

        char* base = reinterpret_cast<char*>(this->memory);
        char* objPtr = base + addr;
        char* blockStart = reinterpret_cast<char*>(round8_down(reinterpret_cast<std::uintptr_t>(objPtr - sizeof(block_header))));

        block_header* block = reinterpret_cast<block_header*>(blockStart);

        static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
        const std::uint64_t blockMeta = block->meta.load(std::memory_order_acquire);
        const std::uint64_t blockFlags = unpack_block_flags(blockMeta);

        const bool isAtomic = blockFlags & K_BLOCK_ATOMIC;
        const bool isAtomicRef = blockFlags & K_BLOCK_ATOMIC_REF;

        if (isAtomic) {
            auto* obj = reinterpret_cast<std::atomic<T>*>(objPtr);
            obj->fetch_add(amount, std::memory_order_release);
        } else if (isAtomicRef) {
            std::atomic_ref<T> ref(*reinterpret_cast<T*>(objPtr));
            ref.fetch_add(amount, std::memory_order_release);
        } else {
            heap_header* head = reinterpret_cast<heap_header*>(this->memory);
            pthread_mutex_lock(&head->heapMutex);
            if (errno == EOWNERDEAD) {
                pthread_mutex_consistent(&head->heapMutex);
            }
            *reinterpret_cast<T*>(objPtr) += amount;
            pthread_mutex_unlock(&head->heapMutex);
        }

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

    template<typename T>
    bool buffer_store(std::size_t dest, const T* src, std::size_t n) {
        static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
        if (!verify_shm(&this->memory, &this->sstat)) {
            return false;
        }

        char* base = reinterpret_cast<char*>(this->memory);
        char* ptr = base + dest;

        char* blockStart = reinterpret_cast<char*>(round8_down(reinterpret_cast<std::uintptr_t>(ptr - sizeof(block_header))));
        block_header* block = reinterpret_cast<block_header*>(blockStart);

        const std::uint64_t blockMeta = block->meta.load(std::memory_order_acquire);
        const std::uint64_t blockFlags = unpack_block_flags(blockMeta);

        const bool isAtomicRef = blockFlags & K_BLOCK_ATOMIC_REF;
        if (!isAtomicRef) {
            return false;
        }

        T* dst = reinterpret_cast<T*>(ptr);
        for (std::size_t i = 0; i < n; ++i) {
            std::atomic_ref<T> ref(dst[i]);
            ref.store(src[i], std::memory_order_release);
        }

        return true;
    }

    template<typename T>
    bool buffer_load(T* dst, std::size_t src, std::size_t n) {
        static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
        if (!verify_shm(&this->memory, &this->sstat)) {
            return false;
        }

        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = this->read_index<T>(src, i);
        }

        return true;
    }

    bool name_addr(std::size_t addr, std::string name) {
        if (!verify_shm(&this->memory, &this->sstat)) return false;
        if (name.empty()) return false;

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        pthread_mutex_lock(&head->tableMutex);
        if (errno == EOWNERDEAD) {
            pthread_mutex_consistent(&head->tableMutex);
        }
        std::size_t rootAddr = head->table.load(std::memory_order_acquire);

        rb_node root = this->read<rb_node>(rootAddr);
        if (root.key_addr == 0) {
            std::size_t keyAddr = this->allocate_array<char>(name.size());
            this->buffer_store<char>(keyAddr, name.c_str(), name.size());
            root.key_addr  = keyAddr;
            root.key_size  = name.size();
            root.data_addr = addr;
            this->write<rb_node>(rootAddr, root);
            pthread_mutex_unlock(&head->tableMutex);
            return true;
        }

        // BST insertion: find insertion point
        std::size_t curAddr = rootAddr;
        std::size_t parAddr = 0;
        bool wentLeft = false;

        while (curAddr != 0) {
            rb_node cur = this->read<rb_node>(curAddr);
            std::string curKey = rb_read_key(cur);
            parAddr = curAddr;
            if (name < curKey) {
                curAddr = cur.left;
                wentLeft = true;
            } else if (name > curKey) {
                curAddr = cur.right;
                wentLeft = false;
            } else {
                cur.data_addr = addr;
                this->write<rb_node>(curAddr, cur);
                pthread_mutex_unlock(&head->tableMutex);
                return true;
            }
        }

        // Allocate key storage and new red node
        std::size_t keyAddr = this->allocate_array<char>(name.size());
        if (keyAddr == 0) {
            pthread_mutex_unlock(&head->tableMutex);
            return false;
        }
        this->buffer_store<char>(keyAddr, name.c_str(), name.size());

        rb_node newNode{};
        newNode.key_addr  = keyAddr;
        newNode.key_size  = name.size();
        newNode.data_addr = addr;
        newNode.parent    = parAddr;
        newNode.left      = 0;
        newNode.right     = 0;
        newNode.isRed     = true;

        std::size_t newAddr = this->allocate<rb_node>(newNode);
        if (newAddr == 0) {
            pthread_mutex_unlock(&head->tableMutex);
            return false;
        }

        rb_node par = this->read<rb_node>(parAddr);
        if (wentLeft) par.left  = newAddr;
        else          par.right = newAddr;
        this->write<rb_node>(parAddr, par);

        // RB insert fix-up
        std::size_t zAddr = newAddr;
        while (true) {
            rb_node z = this->read<rb_node>(zAddr);
            std::size_t pAddr = z.parent;

            if (pAddr == 0) {
                z.isRed = false;
                this->write<rb_node>(zAddr, z);
                break;
            }

            rb_node p = this->read<rb_node>(pAddr);
            if (!p.isRed) break;

            std::size_t gAddr = p.parent;
            rb_node g = this->read<rb_node>(gAddr);
            bool pIsLeft = (g.left == pAddr);
            std::size_t uAddr = pIsLeft ? g.right : g.left;
            bool uncleRed = (uAddr != 0 && this->read<rb_node>(uAddr).isRed);

            if (uncleRed) {
                // Case 1: uncle red → recolor and propagate up
                p.isRed = false;
                this->write<rb_node>(pAddr, p);
                rb_node u = this->read<rb_node>(uAddr);
                u.isRed = false;
                this->write<rb_node>(uAddr, u);
                g.isRed = true;
                this->write<rb_node>(gAddr, g);
                zAddr = gAddr;
                continue;
            }

            if (pIsLeft) {
                if (p.right == zAddr) {
                    // Case 2: inner child → left rotate at p, reducing to Case 3
                    rb_rotate_left(pAddr, head);
                    zAddr = pAddr;
                    pAddr = this->read<rb_node>(zAddr).parent;
                    p     = this->read<rb_node>(pAddr);
                    gAddr = p.parent;
                    g     = this->read<rb_node>(gAddr);
                }
                // Case 3: outer child → right rotate at g
                p.isRed = false;
                this->write<rb_node>(pAddr, p);
                g.isRed = true;
                this->write<rb_node>(gAddr, g);
                rb_rotate_right(gAddr, head);
            } else {
                if (p.left == zAddr) {
                    // Case 2 mirror: inner child → right rotate at p
                    rb_rotate_right(pAddr, head);
                    zAddr = pAddr;
                    pAddr = this->read<rb_node>(zAddr).parent;
                    p     = this->read<rb_node>(pAddr);
                    gAddr = p.parent;
                    g     = this->read<rb_node>(gAddr);
                }
                // Case 3 mirror: outer child → left rotate at g
                p.isRed = false;
                this->write<rb_node>(pAddr, p);
                g.isRed = true;
                this->write<rb_node>(gAddr, g);
                rb_rotate_left(gAddr, head);
            }
            break;
        }

        pthread_mutex_unlock(&head->tableMutex);
        return true;
    }

    bool unname_addr(std::string name) {
        if (!verify_shm(&this->memory, &this->sstat)) return false;
        if (name.empty()) return false;

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        pthread_mutex_lock(&head->tableMutex);
        if (errno == EOWNERDEAD) {
            pthread_mutex_consistent(&head->tableMutex);
        }
        std::size_t rootAddr = head->table.load(std::memory_order_acquire);

        if (this->read<rb_node>(rootAddr).key_addr == 0) {
            pthread_mutex_unlock(&head->tableMutex);
            return false;
        }

        // Step 1: BST search for the key
        std::size_t curAddr = rb_find(name);
        if (curAddr == 0) {
            pthread_mutex_unlock(&head->tableMutex);
            return false;
        }

        // Step 2: Two-children → swap data with in-order successor, then delete successor
        // The successor node has at most a right child; its key is NOT freed here because
        // ownership transfers to the target node — track this with freeDelKey.
        bool freeDelKey = true;
        {
            rb_node target = this->read<rb_node>(curAddr);
            if (target.left != 0 && target.right != 0) {
                std::size_t succAddr = target.right;
                rb_node succ = this->read<rb_node>(succAddr);
                while (succ.left != 0) {
                    succAddr = succ.left;
                    succ     = this->read<rb_node>(succAddr);
                }
                this->free(target.key_addr);    // free the target's old key
                target.key_addr  = succ.key_addr;
                target.key_size  = succ.key_size;
                target.data_addr = succ.data_addr;
                this->write<rb_node>(curAddr, target);
                curAddr    = succAddr;
                freeDelKey = false; // successor's key now lives in target; don't double-free
            }
        }

        // Step 3: Splice out curAddr (has at most one non-null child)
        rb_node del    = this->read<rb_node>(curAddr);
        std::size_t childAddr = (del.left != 0) ? del.left : del.right;
        std::size_t parAddr   = del.parent;
        bool        delWasRed = del.isRed;

        if (childAddr != 0) {
            rb_node child = this->read<rb_node>(childAddr);
            child.parent  = parAddr;
            this->write<rb_node>(childAddr, child);
        }

        if (parAddr == 0) {
            // Deleting the root
            if (childAddr != 0) {
                // Child becomes new root
                head->table.store(childAddr, std::memory_order_release);
                rb_node child = this->read<rb_node>(childAddr);
                child.isRed  = false;
                child.parent = 0;
                this->write<rb_node>(childAddr, child);
                if (freeDelKey) this->free(del.key_addr);
                this->free(curAddr);
            } else {
                // Last node in the tree: reset in place as empty sentinel so head->table stays valid
                std::size_t oldKey = freeDelKey ? del.key_addr : 0;
                del.key_addr  = 0;
                del.key_size  = 0;
                del.data_addr = 0;
                del.left      = 0;
                del.right     = 0;
                del.parent    = 0;
                del.isRed     = false;
                this->write<rb_node>(curAddr, del);
                if (oldKey != 0) this->free(oldKey);
            }
            pthread_mutex_unlock(&head->tableMutex);
            return true;
        }

        // Unlink from parent
        rb_node par = this->read<rb_node>(parAddr);
        if (par.left == curAddr) par.left  = childAddr;
        else                     par.right = childAddr;
        this->write<rb_node>(parAddr, par);

        if (freeDelKey) this->free(del.key_addr);
        this->free(curAddr);

        // No fix-up needed if deleted node was red
        if (delWasRed) {
            pthread_mutex_unlock(&head->tableMutex);
            return true;
        }

        // Red child absorbs the double-black
        if (childAddr != 0) {
            rb_node child = this->read<rb_node>(childAddr);
            if (child.isRed) {
                child.isRed = false;
                this->write<rb_node>(childAddr, child);
                pthread_mutex_unlock(&head->tableMutex);
                return true;
            }
        }

        // Double-black fix-up
        std::size_t xAddr = childAddr;
        while (parAddr != 0) {
            par = this->read<rb_node>(parAddr);
            bool xIsLeft = (par.left == xAddr);
            std::size_t sibAddr = xIsLeft ? par.right : par.left;

            // Case 1: red sibling → rotate to reduce to black-sibling cases
            if (sibAddr != 0 && this->read<rb_node>(sibAddr).isRed) {
                rb_node sib = this->read<rb_node>(sibAddr);
                sib.isRed = false;
                this->write<rb_node>(sibAddr, sib);
                par.isRed = true;
                this->write<rb_node>(parAddr, par);
                if (xIsLeft) rb_rotate_left(parAddr, head);
                else         rb_rotate_right(parAddr, head);
                par     = this->read<rb_node>(parAddr);
                sibAddr = xIsLeft ? par.right : par.left;
            }

            // Gather sibling's children colours
            std::size_t sibLeftAddr = 0, sibRightAddr = 0;
            bool        sibLeftRed  = false, sibRightRed = false;
            rb_node     sib{};
            if (sibAddr != 0) {
                sib = this->read<rb_node>(sibAddr);
                sibLeftAddr  = sib.left;
                sibRightAddr = sib.right;
                if (sibLeftAddr  != 0) sibLeftRed  = this->read<rb_node>(sibLeftAddr).isRed;
                if (sibRightAddr != 0) sibRightRed = this->read<rb_node>(sibRightAddr).isRed;
            }

            // Case 2: both nephews black → colour sibling red, push double-black up
            if (!sibLeftRed && !sibRightRed) {
                if (sibAddr != 0) { sib.isRed = true; this->write<rb_node>(sibAddr, sib); }
                if (par.isRed) {
                    par.isRed = false;
                    this->write<rb_node>(parAddr, par);
                    break;
                }
                xAddr   = parAddr;
                parAddr = par.parent;
                continue;
            }

            // Cases 3 & 4
            par = this->read<rb_node>(parAddr);
            sib = this->read<rb_node>(sibAddr);

            if (xIsLeft) {
                if (!sibRightRed) {
                    // Case 3: inner nephew red → right-rotate sibling, reducing to Case 4
                    rb_node sl = this->read<rb_node>(sibLeftAddr);
                    sl.isRed = false;
                    this->write<rb_node>(sibLeftAddr, sl);
                    sib.isRed = true;
                    this->write<rb_node>(sibAddr, sib);
                    rb_rotate_right(sibAddr, head);
                    par          = this->read<rb_node>(parAddr);
                    sibAddr      = par.right;
                    sib          = this->read<rb_node>(sibAddr);
                    sibRightAddr = sib.right;
                }
                // Case 4: outer nephew red → left-rotate parent
                sib.isRed = par.isRed;
                this->write<rb_node>(sibAddr, sib);
                par.isRed = false;
                this->write<rb_node>(parAddr, par);
                if (sibRightAddr != 0) {
                    rb_node sr = this->read<rb_node>(sibRightAddr);
                    sr.isRed = false;
                    this->write<rb_node>(sibRightAddr, sr);
                }
                rb_rotate_left(parAddr, head);
            } else {
                if (!sibLeftRed) {
                    // Case 3 mirror: inner nephew red → left-rotate sibling
                    rb_node sr = this->read<rb_node>(sibRightAddr);
                    sr.isRed = false;
                    this->write<rb_node>(sibRightAddr, sr);
                    sib.isRed = true;
                    this->write<rb_node>(sibAddr, sib);
                    rb_rotate_left(sibAddr, head);
                    par         = this->read<rb_node>(parAddr);
                    sibAddr     = par.left;
                    sib         = this->read<rb_node>(sibAddr);
                    sibLeftAddr = sib.left;
                }
                // Case 4 mirror: outer nephew red → right-rotate parent
                sib.isRed = par.isRed;
                this->write<rb_node>(sibAddr, sib);
                par.isRed = false;
                this->write<rb_node>(parAddr, par);
                if (sibLeftAddr != 0) {
                    rb_node sl = this->read<rb_node>(sibLeftAddr);
                    sl.isRed = false;
                    this->write<rb_node>(sibLeftAddr, sl);
                }
                rb_rotate_right(parAddr, head);
            }
            break;
        }

        pthread_mutex_unlock(&head->tableMutex);
        return true;
    }

    std::size_t find(std::string name) {
        if (!verify_shm(&this->memory, &this->sstat)) return 0;
        if (name.empty()) return 0;

        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        pthread_mutex_lock(&head->tableMutex);
        if (errno == EOWNERDEAD) {
            pthread_mutex_consistent(&head->tableMutex);
        }
        std::size_t nodeAddr = rb_find(name);
        if (nodeAddr == 0) return 0;

        pthread_mutex_unlock(&head->tableMutex);
        return this->read<rb_node>(nodeAddr).data_addr;
    }

private:
    std::string rb_read_key(const rb_node& node) {
        std::string key;
        key.resize(node.key_size);
        this->buffer_load(&key[0], node.key_addr, node.key_size);
        return key;
    }

    std::size_t rb_find(const std::string& key) {
        heap_header* head = reinterpret_cast<heap_header*>(this->memory);
        std::size_t curAddr = head->table.load(std::memory_order_acquire);
        while (curAddr != 0) {
            rb_node cur = this->read<rb_node>(curAddr);
            std::string curKey = rb_read_key(cur);
            if      (key < curKey) curAddr = cur.left;
            else if (key > curKey) curAddr = cur.right;
            else                   return curAddr;
        }
        return 0;
    }

    std::size_t rb_rotate_left(std::size_t nAddr, heap_header* head) {
        rb_node n = this->read<rb_node>(nAddr);
        std::size_t rAddr = n.right;
        rb_node r = this->read<rb_node>(rAddr);

        n.right = r.left;
        if (r.left != 0) {
            rb_node rl = this->read<rb_node>(r.left);
            rl.parent = nAddr;
            this->write<rb_node>(r.left, rl);
        }
        r.parent = n.parent;
        if (n.parent == 0) {
            head->table.store(rAddr, std::memory_order_release);
        } else {
            rb_node p = this->read<rb_node>(n.parent);
            if (p.left == nAddr) p.left = rAddr;
            else                 p.right = rAddr;
            this->write<rb_node>(n.parent, p);
        }
        r.left   = nAddr;
        n.parent = rAddr;
        this->write<rb_node>(nAddr, n);
        this->write<rb_node>(rAddr, r);
        return rAddr;
    }

    std::size_t rb_rotate_right(std::size_t nAddr, heap_header* head) {
        rb_node n = this->read<rb_node>(nAddr);
        std::size_t lAddr = n.left;
        rb_node l = this->read<rb_node>(lAddr);

        n.left = l.right;
        if (l.right != 0) {
            rb_node lr = this->read<rb_node>(l.right);
            lr.parent = nAddr;
            this->write<rb_node>(l.right, lr);
        }
        l.parent = n.parent;
        if (n.parent == 0) {
            head->table.store(lAddr, std::memory_order_release);
        } else {
            rb_node p = this->read<rb_node>(n.parent);
            if (p.left == nAddr) p.left = lAddr;
            else                 p.right = lAddr;
            this->write<rb_node>(n.parent, p);
        }
        l.right  = nAddr;
        n.parent = lAddr;
        this->write<rb_node>(nAddr, n);
        this->write<rb_node>(lAddr, l);
        return lAddr;
    }

    template<typename T>
    void write(char* objPtr, block_header* block,  const T& val) {
        static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
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
        static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
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