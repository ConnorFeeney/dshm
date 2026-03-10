#include "dshm/shared.h"
#include <iostream>

int main() {
    shared_heap h("test");
    std::size_t obj = h.allocate_array<int>(5);

    h.write_index<int>(obj, 0, 1);
    h.write_index<int>(obj, 2, 5);
    h.write_index<int>(obj, 3, 10);

    std::cout << "ADDR[" << obj << "]: [0]: " << h.read_index<int>(obj, 0) << std::endl;
    std::cout << "ADDR[" << obj << "]: [2]: " << h.read_index<int>(obj, 2) << std::endl;
    std::cout << "ADDR[" << obj << "]: [3]: " << h.read_index<int>(obj, 3) << std::endl;

    h.free(obj);

    return 0;
}