#include "m61.hh"
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <sys/mman.h>
#include <map>
 
 
struct m61_memory_buffer {
   char* buffer;
   size_t pos = 0;
   size_t size = 8 << 20; /* 8 MiB */
 
   m61_memory_buffer();
   ~m61_memory_buffer();
};
 
static m61_memory_buffer default_buffer;

m61_memory_buffer::m61_memory_buffer() {
 
    void* buf = mmap(nullptr,    // Place the buffer at a random address
        this->size,              // Buffer should be 8 MiB big
        PROT_WRITE,              // We want to read and write the buffer
        MAP_ANON | MAP_PRIVATE, -1, 0);
                                // We want memory freshly allocated by the OS
    assert(buf != MAP_FAILED);
    this->buffer = (char*) buf;
 
}
 
m61_memory_buffer::~m61_memory_buffer() {
    munmap(this->buffer, this->size);
}


struct mem_track
{
    size_t sz; // size of allocation
    size_t padding;
    size_t total_size;
    const char* file; // file from which allocation was called
    long line; // line from which allocation was called
};

// Allocation map  to track active memory that is being allocated
// Key: Pointer
// Value: Size of pointer
std::map<void*, mem_track> active_map;

// Free memory map to track  memory that has been freed
// Key: Pointer
// Value: Size of pointer
static std::map<void*, size_t> free_memory; 

using free_memory_iter = std::map<void*, size_t>::iterator; // used for coalescing

using active_map_iter = std::map<void*, mem_track>::iterator; // used for leak

static int max_align = alignof(max_align_t);
const unsigned long random_int = 0xFEEEEF11;

/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.
 
 // initialize all stats to 0
static m61_statistics gstats = {
    .nactive = 0,
    .active_size = 0,
    .ntotal = 0,
    .total_size = 0,
    .nfail = 0,
    .fail_size = 0,
    .heap_min = 0,
    .heap_max = 0
};
 

 // frame from sample on pset 1 page
static void* m61_find_free_space(mem_track allocation) {
    // Try to reuse freed
    for (auto it : free_memory) {
        if (it.second >= allocation.total_size && allocation.sz > 0) {
            void* ptr = it.first;
            free_memory.erase(it.first);
            if (it.second > allocation.total_size) {
                free_memory.insert({(void*) ((uintptr_t) it.first + allocation.total_size), it.second - allocation.total_size});
            }
            return ptr;
        }
    }
    // otherwise fail
    gstats.nfail++;
    gstats.fail_size += allocation.sz;
    return nullptr;
}

void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    // Your code here.
    
    if (sz > default_buffer.size || sz == 0) {
        // Not enough space in default buffer
        ++gstats.nfail;
        gstats.fail_size += sz;
        return nullptr;
    }

    // claim the next `sz` bytes
    size_t padd = 0;
    if (sz + sizeof(random_int) % max_align != 0) {
        padd = max_align - (sz + sizeof(random_int)) % max_align; // padding based on alignment
    }
    size_t total_size = sz + padd + sizeof(random_int);

    if (active_map.empty() && free_memory.empty()) {
        // if empty, add the entire buffer to free map
        default_buffer.pos = default_buffer.buffer[0] + default_buffer.size;
        free_memory.insert({&default_buffer.buffer[0], default_buffer.size});
    }

    mem_track allocation = {sz, padd, total_size, file, line};
    void* ptr = m61_find_free_space(allocation);
    
    if (ptr) {

        if (!gstats.ntotal) {
            gstats.heap_min = (uintptr_t) ptr;
            gstats.heap_max = (uintptr_t) ptr + sz;
        }
        if ((uintptr_t) ptr + sz > gstats.heap_max){ //checks if heap max is at greatest value
            gstats.heap_max = (uintptr_t) ptr + sz;
        }
        else if ((uintptr_t) ptr < gstats.heap_min){ //checks if heap min is at lowest value
            gstats.heap_min = (uintptr_t) ptr;
        }

        ++gstats.ntotal;
        ++gstats.nactive;
        gstats.total_size += sz;
        gstats.active_size += sz;
        active_map.insert({ptr, allocation});
        //write down the random piece to memory
        memcpy((unsigned long*)((uintptr_t) ptr + sz), &random_int, sizeof(random_int));


        int alignment = default_buffer.pos / alignof(std::max_align_t); 
        default_buffer.pos = (alignment + 1) * alignof(std::max_align_t);
        assert(default_buffer.pos % alignof(std::max_align_t) == 0);
    }
    
    return ptr;
}

 // Coalescing information.
bool can_coalesce_up(free_memory_iter it) {
    assert(it != free_memory.end());
    // Check if next sample exists
    auto next = it;
    ++next;
    if (next == free_memory.end()) {
        return false;
    }

    return (uintptr_t) it->first + (uintptr_t) it->second == (uintptr_t) next->first;
}

void coalesce_up(free_memory_iter it) {
    assert(can_coalesce_up(it));
    auto next = it;
    ++next;
    it->second += next->second;
    free_memory.erase(next);
}


bool can_coalesce_down(free_memory_iter it) {
    assert(it != free_memory.end());
    // Check if previous sample exists
    if (it == free_memory.begin()) {
        return false;
    }

    auto prev = it;
    --prev;
    return (uintptr_t) prev->first + 
    (uintptr_t) prev->second == (uintptr_t) it->first;
}


void final_coalesce(void* ptr, size_t sz) { //uses prior info to check if memory alloc can be coalesced
    // Strategy: first insert, then coalesce
    free_memory.insert({ptr, sz});
    auto it = free_memory.find(ptr);
    while (can_coalesce_down(it)) {
        --it;
    }
    while (can_coalesce_up(it)) {
        coalesce_up(it);
    } 
}


/// m61_free(ptr, file, line)
///    Frees the memory allocation pointed to by `ptr`. If `ptr == nullptr`,
///    does nothing. Otherwise, `ptr` must point to a currently active
///    allocation returned by `m61_malloc`. The free was called at location
///    `file`:`line`.
/// map would be used here, store value before freeing
 


void m61_free(void* ptr, const char* file, int line) {
    // avoid uninitialized variable warnings
    (void) ptr, (void) file, (void) line;
    
    if (ptr == nullptr) {
        return;
    }

    if ((uintptr_t) ptr < gstats.heap_min || (uintptr_t) ptr > gstats.heap_max) {
        fprintf(stderr, "MEMORY BUG %s%i: invalid free of pointer %p, not in heap\n", file, line, ptr);
        abort();
    }

    else if (free_memory.find(ptr) != free_memory.end()){
        fprintf(stderr, "MEMORY BUG: %s:%d: invalid free of pointer %p, double free\n", file, line, ptr);
        abort();
    }

    else if (active_map.find(ptr) == active_map.end()) {
        fprintf(stderr, "MEMORY BUG: %s:%d: invalid free of pointer %p, not allocated\n", file, line, ptr);
        for (active_map_iter itr = active_map.begin(); itr != active_map.end(); ++itr){
            // check if ptr is between existing ptr and existing ptr + sz
            if ((uintptr_t) itr -> first < (uintptr_t) ptr && 
            (uintptr_t) itr -> first + active_map[itr -> first].sz > (uintptr_t) ptr){
                    fprintf(stderr,"%s:%li: %p is %li bytes inside a %li byte region allocated here\n",
                    itr -> second.file,
                    itr -> second.line,
                    ptr,
                    (uintptr_t) ptr - (uintptr_t) itr -> first,
                    itr -> second.sz);
            }
        }
        abort();
    }

    // I am aware that I will probably not get the grades for this as 
    // it was not in my original submission, however to make all my tests 
    // green and myself happy I tried fixing my test43-45. (getting the grade would be nice :D)
    else if (memcmp((void*)((uintptr_t)ptr + active_map.find(ptr)->second.sz), &random_int, sizeof(unsigned long)) != 0
        && (uintptr_t)ptr + (uintptr_t)active_map.find(ptr)->second.sz % max_align != 0) {
        fprintf(stderr, "%s %p\n", "MEMORY BUG: detected wild write during free of pointer", ptr);
        abort();
    }
    
    // Decrease active size by sz of active map
    gstats.nactive--;
    gstats.active_size -= active_map[ptr].sz;

    mem_track allocation = active_map[ptr];
    active_map.erase(ptr);
    // adds ptr information to freed map 
    final_coalesce(ptr, allocation.total_size);
    
}


/// m61_calloc(count, sz, file, line)
///    Returns a pointer a fresh dynamic memory allocation big enough to
///    hold an array of `count` elements of `sz` bytes each. Returned
///    memory is initialized to zero. The allocation request was at
///    location `file`:`line`. Returns `nullptr` if out of memory; may
///    also return `nullptr` if `count == 0` or `size == 0`.
 
void* m61_calloc(size_t count, size_t sz, const char* file, int line) {
    // Your code here (to fix test019).
    if (count != 0 && (count * sz) / count != sz){
        gstats.nfail++;
        return nullptr;
    }
    void* ptr = m61_malloc(count * sz, file, line);
    if (ptr) {
        memset(ptr, 0, count * sz);
    }
    return ptr;
}
 

/// m61_realloc(ptr, sz, file, line)
///    Changes the size of the dynamic allocation pointed to by `ptr`
///    to hold at least `sz` bytes. If the existing allocation cannot be
///    enlarged, this function makes a new allocation, copies as much data
///    as possible from the old allocation to the new, and returns a pointer
///    to the new allocation. If `ptr` is `nullptr`, behaves like
///    `m61_malloc(sz, file, line). `sz` must not be 0. If a required
///    allocation fails, returns `nullptr` without freeing the original
///    block.

void* m61_realloc(void* ptr, size_t sz, const char* file, long line) {
    // Check if the input pointer is null.
    if (ptr == nullptr) {
        // If the pointer is null, simply allocate a new block using m61_malloc.
        return m61_malloc(sz, file, line);
    }

    // Allocate a new block using m61_malloc.
    void* new_ptr = m61_malloc(sz, file, line);

    // Copy as much data as possible from the old block to the new block.
    memcpy(new_ptr, ptr, sz);

    // Free the old block using m61_free.
    m61_free(ptr, file, line);

    // Return a pointer to the new block.
    return new_ptr;
}

/// m61_get_statistics()
///    Return the current memory statistics.
m61_statistics m61_get_statistics() {
    // Your code here.
    // The handout code sets all statistics to enormous numbers.
    m61_statistics stats;
    memset(&stats, 0, sizeof(m61_statistics));
    
    return gstats;
}
 
/// m61_print_statistics()
///    Prints the current memory statistics.
 
void m61_print_statistics() {
 m61_statistics stats = m61_get_statistics();
 printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
        stats.nactive, stats.ntotal, stats.nfail);
 printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
        stats.active_size, stats.total_size, stats.fail_size);
}
 
 
/// m61_print_leak_report()
///    Prints a report of all currently-active allocated blocks of dynamic
///    memory.
void m61_print_leak_report() {
 // Your code here.
   for (active_map_iter itr = active_map.begin(); itr != active_map.end(); ++itr){
       fprintf(stdout,"LEAK CHECK: %s:%li: allocated object %p with size %li\n",
        itr -> second.file,
        (long) itr -> second.line,
        (unsigned long*) itr -> first,
        itr -> second.sz);
      
   }

}
