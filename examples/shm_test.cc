#include <iostream>
#include <system_error>

#include "dshm/shared_buffer.h"

int main() {
    std::error_code ec;
    dshm::SharedBuffer buffer("/dshm_test", dshm::OWN | dshm::READ | dshm::WRITE, ec);
    if (ec) {
        std::cerr << "Failed to open shared buffer: " << ec.value() << "\n";
        return 1;
    }

    while (true) {
        std::cout << "Enter a single character (r = read, e = exit): ";
        char ch = '\0';
        if (!(std::cin >> ch)) {
            return 1;
        }

        if (ch == 'e') {
            break;
        }

        if (ch == 'r') {
            const char* data = buffer.read(1, 0);
            if (!data) {
                std::cerr << "Read failed" << std::endl;
                return 1;
            }
            std::cout << "Read: " << *data << std::endl;
            continue;
        }

        std::size_t written = buffer.write(&ch, 1, 0);
        if (written != 1) {
            std::cerr << "Write failed" << std::endl;
            return 1;
        }
        std::cout << "Wrote: " << ch << std::endl;

        std::cout << "Press r then Enter to read it back: ";
        char readCmd = '\0';
        if (!(std::cin >> readCmd)) {
            return 1;
        }
        if (readCmd == 'r') {
            const char* data = buffer.read(1, 0);
            if (!data) {
                std::cerr << "Read failed" << std::endl;
                return 1;
            }
            std::cout << "Read: " << *data << std::endl;
        }
    }

    return 0;
}
