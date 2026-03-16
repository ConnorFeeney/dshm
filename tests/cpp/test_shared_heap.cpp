#include "doctest/doctest.h"
#include "dshm/shared_heap.h"

TEST_CASE("Test Shared Heap") {
    shared_heap sh("testHeap");

    SUBCASE("Single Variable") {
        constexpr int initial = 100;
        constexpr int writeVal = 200;

        std::size_t addr = sh.allocate<int>(initial);
        REQUIRE(addr != 0);

        CHECK(sh.read<int>(addr) == initial);

        CHECK(sh.write<int>(addr, writeVal));

        CHECK(sh.read<int>(addr) == writeVal);

        REQUIRE(sh.free(addr));
    }

    SUBCASE("Array Test") {
        constexpr std::size_t arraySize = 3;

        std::size_t addr = sh.allocate_array<int>(arraySize);
        REQUIRE(addr != 0);

        CHECK(sh.write_index<int>(addr, 0, 5));
        CHECK(sh.write_index<int>(addr, 1, 10));
        CHECK(sh.write_index<int>(addr, 2, 15));

        CHECK(sh.read_index<int>(addr, 0) == 5);
        CHECK(sh.read_index<int>(addr, 1) == 10);
        CHECK(sh.read_index<int>(addr, 2) == 15);

        CHECK(sh.write_index(addr, 0, 10));
        CHECK(sh.write_index(addr, 1, 15));
        CHECK(sh.write_index(addr, 2, 5));

        CHECK(sh.read_index<int>(addr, 0) == 10);
        CHECK(sh.read_index<int>(addr, 1) == 15);
        CHECK(sh.read_index<int>(addr, 2) == 5);

        REQUIRE(sh.free(addr));
    }

    SUBCASE("fetchAdd Test") {
        constexpr int initial = 100;
        constexpr int increment = 200;

        std::size_t addr = sh.allocate<int>(initial);
        REQUIRE(addr != 0);

        CHECK(sh.fetch_add<int>(addr, increment));

        CHECK(sh.read<int>(addr) == initial + increment);

        REQUIRE(sh.free(addr));
    }

    SUBCASE("Search Test") {
        constexpr int initial = 100;

        std::size_t addr1 = sh.allocate<int>(initial);
        REQUIRE(sh.name_addr(addr1, "valueABC"));
        REQUIRE(addr1 != 0);
        std::size_t addr2 = sh.allocate<int>(initial);
        REQUIRE(sh.name_addr(addr2, "valueDEF"));
        REQUIRE(addr2 != 0);
        std::size_t addr3 = sh.allocate<int>(initial);
        REQUIRE(sh.name_addr(addr3, "valueGHI"));
        REQUIRE(addr3 != 0);

        std::size_t found1 = sh.find("valueABC");
        std::size_t found2 = sh.find("valueDEF");
        std::size_t found3 = sh.find("valueGHI");

        CHECK(found1 == addr1);
        CHECK(found2 == addr2);
        CHECK(found3 == addr3);

        CHECK(sh.free(addr1));
        CHECK(sh.free(addr2));
        CHECK(sh.free(addr3));
    }
}