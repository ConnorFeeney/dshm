#include "dshm/shared_object.h"

int main() {
    shared_object<int> testObj = make_shared_obj<int>("veryCoolHeap");
    testObj = 2;

    std::cout << testObj << std::endl;
    std::cout << testObj.addr() << std::endl;
    
    std::cin.get();
    return 0;
}