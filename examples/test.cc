#include "dshm/shared_object.h"

int main() {
    shared_object<std::uint64_t> testObj = make_shared_obj<std::uint64_t>("veryCoolHeap");
    testObj = 2;

    std::cout << testObj << std::endl;
    std::cout << testObj.addr() << std::endl;

    std::cin.get();
    return 0;
}