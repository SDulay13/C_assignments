#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cstdlib>

//TESTING REALLOC

int main() {
    // Test resizing a non-null allocation to a larger size.
    void* ptr = m61_malloc(10, __FILE__, __LINE__);
    memcpy(ptr, "CS61!", 5);
    ptr = m61_realloc(ptr, 20, __FILE__, __LINE__);
    assert(memcmp(ptr, "CS61!", 5) == 0);

    // Test resizing a non-null allocation to a smaller size.
    ptr = m61_realloc(ptr, 5, __FILE__, __LINE__);
    assert(memcmp(ptr, "CS61!", 5) == 0);

    // Test resizing a non-null allocation to the same size.
    ptr = m61_realloc(ptr, 5, __FILE__, __LINE__);
    assert(memcmp(ptr, "CS61!", 5) == 0);

    // Test resizing a non-null allocation to 0 size.
    ptr = m61_realloc(ptr, 0, __FILE__, __LINE__);
    assert(ptr == nullptr);

    // Test resizing a null pointer.
    ptr = m61_realloc(nullptr, 10, __FILE__, __LINE__);
    assert(ptr != nullptr);
}

