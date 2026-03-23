# DSHM

A header only C++/Python POISX dynamic shared memory library for creating **shared objects**, variables that can be saftley accessed and modified by multiple unrelated processes.

## Highlights

- Cross-process shared objects for Linux
- Safe concurrent memory access using a hybrid synchronization approach (combination of split mutexing and atomic instructions)
- C++ core with Python bindings

## Overview

### The Problem

Sharing a mutable state across unrelated processes is a challenging problem in system programming. Approaches such as file-based communication are often slow and error-prone, while naive use of shared memory can easily lead to race conditions or data corruption. Developers often need to implement complex locking and/or messaging protocols themselves, adding performance overhead and code complexity.

### The Solution

DSHM's `shared_object<T>` addresses these challenges by providing a library-level abstraction for shared state variables. It guarantees **memory safety** and **atomic visibility** of updates across processes, ensuring that concurrent reads and writes do not corrupt memory. By combining fine-grained mutexing with atomic instructions, the library minimizes contention while keeping operations fast.

## Usage

### C++ Example

```C++
#include <iostream>
#include "dshm/types/shared_object.h"

int main() {
    /**
    Create a shared int named "sharedInt" withing the namespace "namespace"
    If the variable already exsits in the namespace sharedInt will refrence the same variable
    */
    dshm::shared_object<int> sharedInt = dshm::make_or_find("namespace", "sharedInt");

    sharedInt = 100; // Update the shared int value (will reflect in other processes)

    // Print out the value and shared address of sharedInt
    std::cout << "Shared int value: " << sharedInt << std::endl;
    std::cout << "Shared int address: " << sharedInt.addr() << std::endl;

    // Free the memory consumed by sharedInt (Future writes are undefined)
    sharedInt.destroy();
}
```

### Python

```Python
import dshmpy
"""
Create a shared int named 'sharedInt' withing the namespace 'namespace'
If the variable already exsits in the namespace sharedInt will refrence the same variable
"""
sharedInt = dshmpy.make_or_find("namespace", "sharedInt", dshmpy.int32)

sharedInt.set(100) # Update the shared int value (will reflect in other processes)

# Print out the value and shared address of sharedInt
print(sharedInt)
print(sharedInt.addr())

# Free the memory consumed by sharedInt (Future writes are undefined)
sharedInt.destroy()

```

## Installation

### Requirments (Python)

- CMake
- Clang
- Ninja
- Python3

### Instructions

#### C++

C++ is header only, simply copy the headers into your project, or clone the entire project and make the include directory avalible.

#### Python

Run the following.

```bash
git clone https://github.com/ConnorFeeney/dshm.git
cd dshm
pip install .
```

## Security Model

Uses POISX shared memory(/dev/shm) with 0600 permission bits (owner only access) meaing the unrelated processes must be run by the same owner. All other read write protections are left to the user
