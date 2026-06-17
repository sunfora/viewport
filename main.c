#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <stdio.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

extern char _etext; // End of text (code) segment
extern char _edata; // End of initialized data segment
extern char _end;   // End of uninitialized data (.bss) / Start of Heap
extern void *__libc_stack_end; 

typedef uint8_t byte;
typedef size_t  size;

#define    B(x) ((uintptr_t)(x) <<  0)
#define   KB(x) ((uintptr_t)(x) << 10)
#define   MB(x) ((uintptr_t)(x) << 20)
#define   GB(x) ((uintptr_t)(x) << 30)
#define   TB(x) ((uintptr_t)(x) << 40)


#ifdef PRODUCTION
  #define BASE_ADDRESS    NULL
  #define BASE_SPACE_RESERVED  /* TODO(ivan): find out how much later */
#else
  #define BASE_ADDRESS   ((void*)(TB(2 * 16)))
  #define DEBUG_PLACE    ((void*)(TB(3 * 16)))

  #define BASE_SPACE_RESERVED GB(1)
  #define DBGP_SPACE_RESERVED GB(1)
#endif

inline size page_size() {
  return sysconf(_SC_PAGESIZE);
}

void dump_pages_inside_region(void* memory, size length) {
  const size system_page_size         = page_size();
  const size max_pages_per_cycle      = 1024;
  const size default_length_per_cycle = max_pages_per_cycle * system_page_size;

  int   block_status = 0;
  size  block_size   = 0;
  void* block_addr   = 0;

  byte  mincore_vec [max_pages_per_cycle];

  void* mincore_start      = memory;
  size  space_left_to_read = length;
  
  const char* used_template     = "%p\t%d\t[   used   ]\n";
  const char* not_used_template = "%p\t%d\t[ not_used ]\n";
  const char* unmapped_template = "%p\t%d\t[ unmapped ]\n";

  while (space_left_to_read) {
    // here do organize fetch from mincore
    size mincore_length = default_length_per_cycle;
    if (mincore_length > space_left_to_read) {
      mincore_length = space_left_to_read;
    }

    size pages_read   = 0;
    bool chunk_failed = false;

restart:
    chunk_failed = mincore(mincore_start, mincore_length, mincore_vec) == -1;
    pages_read   = mincore_length / system_page_size;

    if (chunk_failed) {
      if (errno != ENOMEM) {
        printf("[dump_error] mincore critical error: %d\n", errno);
        return;
      } else {
        // we are gonna handle this thing
        errno = 0;
      }
      // we cannot understand anything could be anything 
      // so instead we just restart
      // and try again with a length of 1 page
      if (pages_read > 1) {
        mincore_length = system_page_size;
        goto restart;
      } 
    }
    
    // go over eached fetched page
    // and if it flip flops print the prev
    // and update 
    for (size page_id = 0; page_id < pages_read; page_id += 1) {
      int this_page_status;
      if (chunk_failed) {
        this_page_status = -1;
      } else {
        this_page_status = (mincore_vec[page_id] & 1);
      }

      bool page_used_status_flipped = (
           block_size > 0
        && this_page_status != block_status
      );

      bool reinitialize = (
           (page_used_status_flipped)
        || (block_size == 0)
      );

      if (page_used_status_flipped) {
        // print the previous
        if (block_status == 0) {
          printf(not_used_template, block_addr, block_size);
        } else if (block_status == 1) {
          printf(used_template, block_addr, block_size);
        } else {
          printf(unmapped_template, block_addr, block_size);
        }
      }

      if (reinitialize) {
        block_status = this_page_status;
        block_size   = 1;
        block_addr   = mincore_start + (page_id * system_page_size);
      } else {
        block_size += 1;
      }
    }

    // here do updates 
    mincore_start      += mincore_length;
    space_left_to_read -= mincore_length;
  }

  // here do print out the remaining things
  // with the last dude
  if (block_size > 0) {
    if (block_status == 0) {
      printf(not_used_template, block_addr, block_size);
    } else if (block_status == 1) {
      printf(used_template, block_addr, block_size);
    } else {
      printf(unmapped_template, block_addr, block_size);
    }
  }
}

void dump_who_sits_in_memory() {
  static char cmd[128];
  sprintf(cmd, "pmap -x %d", getpid()); 
  system(cmd);
}

#define CHUNK_SIZE 256

struct String {
  byte content[CHUNK_SIZE];
  struct String* next;
};

int main() {
  struct rlimit limit;
  getrlimit(RLIMIT_STACK, &limit);
  
  void* main_program_memory = mmap(
    BASE_ADDRESS, 
    BASE_SPACE_RESERVED, 
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_FIXED_NOREPLACE | MAP_ANONYMOUS,
    -1,
    0
  );

  if (main_program_memory == MAP_FAILED) {
    fprintf(stderr, "Memory map for memory has failed\n");
    return 1;
  }

  void* debug_propgram_memory = mmap(
    DEBUG_PLACE,
    DBGP_SPACE_RESERVED, 
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_FIXED_NOREPLACE | MAP_ANONYMOUS,
    -1,
    0
  );

  if (debug_propgram_memory == MAP_FAILED) {
    fprintf(stderr, "Memory map for debug_memory has failed\n");
    return 1;
  }

  printf(" %p\n", main_program_memory);

  printf("    RLIMIT_STACK: %lu bytes\n", limit.rlim_cur);
  printf("    RLIMIT_STACK: %lu mb\n",    limit.rlim_cur / 1024 / 1024);
  printf("__libc_stack_end: %p\n", __libc_stack_end);
  printf("          _etext: %p\n", (void*)&_etext);
  printf("          _edata: %p\n", (void*)&_edata);
  printf("            _end: %p\n", (void*)&_end);
  
  dump_who_sits_in_memory();
  ((char*)main_program_memory)[0] = 'h';
  ((char*)main_program_memory)[1] = 'e';
  ((char*)main_program_memory)[2] = 'l';
  ((char*)main_program_memory)[15 * page_size()] = 'l';
  ((char*)main_program_memory)[16 * page_size()] = 'l';
  ((char*)main_program_memory)[17 * page_size()] = 'l';
  ((char*)main_program_memory)[18 * page_size()] = 'l';
  void* target_addr = (char*)main_program_memory + (16 * page_size()); 
  madvise(target_addr, 2 * page_size(), MADV_DONTNEED);

  puts("dump in the allocated region");
  dump_pages_inside_region(main_program_memory, GB(1));

  puts("dump heap");
  

  size small_large_size = 86 * page_size();
  volatile char* small_ptr = malloc(small_large_size);
  small_ptr[small_large_size - 1] = '4';

  size large_size = MB(256);
  volatile char* large_ptr = malloc(large_size);
  large_ptr[0] = 'r';

  uintptr_t heap_start_addr = (uintptr_t)(&_end);
  heap_start_addr = (
      (heap_start_addr + (page_size() - 1)) 
    & (~(page_size() - 1))
  );

  void* heap_start = (void*) heap_start_addr;
  void* heap_end   = sbrk(0);
  size  heap_len   = (size)((char*)heap_end - (char*)heap_start);
  dump_pages_inside_region(heap_start, heap_len);
  
  puts("dump heap (large_size) ");
  uintptr_t aligned_addr = (uintptr_t)large_ptr & ~(page_size() - 1);
  size aligned_len = large_size + ((uintptr_t)large_ptr - aligned_addr);
  dump_pages_inside_region((void*)aligned_addr, aligned_len);

  puts("dump unmapped gap");
  void* empty_space_start = (void*) (BASE_ADDRESS - 1024 * page_size());
  dump_pages_inside_region(empty_space_start, GB(1));


  puts("dump starting from tcc 1GB");
  dump_pages_inside_region((void*)(0x0000000000400000), GB(1));

  puts("dump stack");
  dump_pages_inside_region(
      (void*)(0x0000800000000000 - limit.rlim_cur), 
      limit.rlim_cur
  );

  alloca(251 * page_size());
  
  char *mem = alloca(MB(6));
  mem[MB(5) - 1] = 'a';

  puts("dump stack (after alloca(7mb); mem[5mb - 1] = 'a')");
  dump_pages_inside_region(
      (void*)(0x0000800000000000 - limit.rlim_cur), 
      limit.rlim_cur
  );

  return 0;
}
