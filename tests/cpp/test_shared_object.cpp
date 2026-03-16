#include "doctest/doctest.h"
#include "dshm/types/shared_object.h"

TEST_CASE("Test Shared Object") {
    SUBCASE("Allocation Test") {
        shared_heap* heap = sheap("testHeap");
        shared_object<int> obj(heap, "obj", 100);

        REQUIRE(obj.addr() != 0);
        CHECK(obj == 100);

        obj.destroy();

        CHECK(obj.addr() == 0);
    }

    SUBCASE("Operator Test") {
        shared_heap* heap = sheap("testHeap");
        shared_object<int> obj(heap, "opObj", 10);

        REQUIRE(obj.addr() != 0);

        // Assignment
        obj = 42;
        CHECK(obj == 42);

        // Addition
        CHECK(obj + 8 == 50);

        // += operator
        obj += 5;
        CHECK(obj == 47);

        // -= operator
        obj -= 7;
        CHECK(obj == 40);

        // Subtraction
        CHECK(obj - 10 == 30);

        // Pre-increment: increments then returns updated object
        ++obj;
        CHECK(obj == 41);

        // Pre-decrement: decrements then returns updated object
        --obj;
        CHECK(obj == 40);

        // Post-increment: returns old value, then increments
        obj = 10;
        CHECK(obj++ == 10);
        CHECK(obj == 11);

        // Post-decrement: returns old value, then decrements
        CHECK(obj-- == 11);
        CHECK(obj == 10);

        // Equality between two shared_objects
        shared_object<int> obj2(heap, "opObj2", 10);
        CHECK(obj == obj2);

        obj2.destroy();
        obj.destroy();
    }

    SUBCASE("Search Test") {
        shared_heap* heap = sheap("testHeap");

        // Create a named object
        shared_object<int> obj(heap, "searchObj", 99);
        REQUIRE(obj.addr() != 0);

        // dshm_make_or_find should find the existing object by name
        shared_object<int> found = dshm_make_or_find<int>("testHeap", "searchObj");
        CHECK(found.addr() != 0);
        CHECK(found == 99);

        // dshm_make_or_find on a new name should allocate a new object
        shared_object<int> created = dshm_make_or_find<int>("testHeap", "newSearchObj");
        CHECK(created.addr() != 0);

        // Addresses should differ since they are distinct objects
        CHECK(found.addr() != created.addr());

        // Construct directly from a known address
        std::size_t knownAddr = obj.addr();
        shared_object<int> fromAddr(heap, knownAddr, "searchObjAlias", shared_object<int>::from_address);
        CHECK(fromAddr.addr() == knownAddr);
        CHECK(fromAddr == 99);

        created.destroy();
        fromAddr.destroy();
        obj.destroy();
    }
}