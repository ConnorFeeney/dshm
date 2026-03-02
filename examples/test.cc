#include "dshm/shared_heap.h"

int main() {
    std::error_code ec;
    shared_heap sh("testheap", ec);
    
    std::size_t obj = sh.allocate<int>(100);
    sh.write_at(obj, 1000);

    std::size_t obj2 = sh.allocate<int>(100);
    sh.write_at(obj2, 1001);

    std::cout << "READ: 0x" << std::hex << obj << " | " << std::dec << sh.read_at<int>(obj) << std::endl;
    std::cout << "READ: 0x" << std::hex << obj2 << " | " << std::dec << sh.read_at<int>(obj2) << std::endl;

    sh.free(obj);
    sh.free(obj2);
    return 0;
}