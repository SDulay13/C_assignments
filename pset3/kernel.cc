#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"
#include "obj/k-firstprocess.h"
#include "atomic.hh"
 
// kernel.cc
//
//    This is the kernel.
 
#define PROC_SIZE 0x40000       // initial state only
 
proc ptable[NPROC];             // array of process descriptors
                               // Note that `ptable[0]` is never used.
proc* current;                  // pointer to currently executing proc
 
#define HZ 100                  // timer interrupt frequency (interrupts/sec)
static atomic<unsigned long> ticks; // # timer interrupts so far
 
 
// Memory state - see `kernel.hh`
physpageinfo physpages[NPAGES];

proc* runqueue_head = nullptr;
proc* runqueue_tail = nullptr;


 
 
[[noreturn]] void schedule();
[[noreturn]] void run(proc* p);
void exception(regstate* regs);
uintptr_t syscall(regstate* regs);
void memshow();
 
 
// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.
 
static void process_setup(pid_t pid, const char* program_name);
 
void kernel_start(const char* command) {
   // initialize hardware
   init_hardware();
   log_printf("Starting WeensyOS\n");
 
   ticks = 1;
   init_timer(HZ);
 
   // clear screen
   console_clear();
 
   // (re-)initialize kernel page table
   for (uintptr_t addr = 0; addr < MEMSIZE_PHYSICAL; addr += PAGESIZE) {
       int perm = PTE_P | PTE_W | PTE_U;
       if (addr == 0) {
           // nullptr is inaccessible even to the kernel
           perm = 0;
       }
      
       else if (addr < PROC_START_ADDR && addr != CONSOLE_ADDR) {
               // kernel/kernel reserved addr -> priv perms
               perm = PTE_P | PTE_W;
           }
 
       // install identity mapping
       int r = vmiter(kernel_pagetable, addr).try_map(addr, perm);
       assert(r == 0); // mappings during kernel_start MUST NOT fail
                       // (Note that later mappings might fail!!)
   }
 
   // set up process descriptors
   for (pid_t i = 0; i < NPROC; i++) {
       ptable[i].pid = i;
       ptable[i].state = P_FREE;
   }
   if (!command) {
       command = WEENSYOS_FIRST_PROCESS;
   }
   if (!program_image(command).empty()) {
       process_setup(1, command);
   }
   else {
       process_setup(1, "allocator");
       process_setup(2, "allocator2");
       process_setup(3, "allocator3");
       process_setup(4, "allocator4");
   }
 
   // Switch to the first process using run()
   run(&ptable[1]);
}
 
 
// kalloc(sz)
//    Kernel physical memory allocator. Allocates at least `sz` contiguous bytes
//    and returns a pointer to the allocated memory, or `nullptr` on failure.
//    The returned pointer’s address is a valid physical address, but since the
//    WeensyOS kernel uses an identity mapping for virtual memory, it is also a
//    valid virtual address that the kernel can access or modify.
//
//    The allocator selects from physical pages that can be allocated for
//    process use (so not reserved pages or kernel data), and from physical
//    pages that are currently unused (`physpages[N].refcount == 0`).
//
//    On WeensyOS, `kalloc` is a page-based allocator: if `sz > PAGESIZE`
//    the allocation fails; if `sz < PAGESIZE` it allocates a whole page
//    anyway.
//
//    The handout code returns the next allocatable free page it can find.
//    It checks all pages. (You could maybe make this faster!)
//
//    The returned memory is initially filled with 0xCC, which corresponds to
//    the `int3` instruction. Executing that instruction will cause a `PANIC:
//    Unhandled exception 3!` This may help you debug.
 
void* kalloc(size_t sz) {
   if (sz > PAGESIZE) {
       return nullptr;
   }
 
   for (uintptr_t pa = 0; pa != MEMSIZE_PHYSICAL; pa += PAGESIZE) {
       if (allocatable_physical_address(pa)
           && physpages[pa / PAGESIZE].refcount == 0) {
           ++physpages[pa / PAGESIZE].refcount;
           memset((void*) pa, 0xCC, PAGESIZE);
           return (void*) pa;
       }
   }
   return nullptr;
}
 
 void sys_page_free(void* ptr) {
    // check if ptr is a valid virtual address
    // and points to a page-aligned physical address
    if (!ptr || ((uintptr_t) ptr % PAGESIZE) != 0) {
        return;
    }
    // find the corresponding physical page
    int physpage = ((uintptr_t) ptr / PAGESIZE);
    if (physpage < 0 || physpage >= (int) NPAGES) {
        return;
    }

    // free the physical page if it is allocated
    if (physpages[physpage].refcount > 0) {
        physpages[physpage].refcount = 0;
    }
}

uintptr_t kill(pid_t pid) {
    // Check if the calling process is trying to kill itself
    if (pid == current->pid) {
        // Set the state of the calling process to P_ZOMBIE
        current->state = P_ZOMBIE;

        // Return success
        return 0;
    }

    // Check if the calling process has the permission to kill the specified process
    if (current->euid != 0 && current->euid != ptable[pid].uid) {
        // Return error if the calling process does not have the permission
        return -1;
    }

    // Set the state of the specified process to P_ZOMBIE
    ptable[pid].state = P_ZOMBIE;

    // Return success
    return 0;
}

// runqueue_add(p)
//    Add process `p` to the run queue.
void runqueue_add(proc* p) {
    // Add the process to the end of the run queue
    p->next = nullptr;
    if (runqueue_tail) {
        runqueue_tail->next = p;
    }
    else {
        runqueue_head = p;
    }
    runqueue_tail = p;
}


// kfree(kptr)
//    Free `kptr`, which must have been previously returned by `kalloc`.
//    If `kptr == nullptr` does nothing.
 
void kfree(void* kptr) {
   (void) kptr;
   assert(physpages[((uintptr_t) kptr) / PAGESIZE].refcount != 0);
       // identify entry in the pages array and free it
   --physpages[((uintptr_t) kptr) / PAGESIZE].refcount;
}
 
 
// process_setup(pid, program_name)
//    Load application program `program_name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.
 
void process_setup(pid_t pid, const char* program_name) {
    proc* p = &ptable[pid];
    p->uid = 0; // Set the user ID of the process to 0 (root)

    // Initialize the effective user ID of the process to 0 (root)
    p->euid = 0;
 
   // initialize empty-page table
   ptable[pid].pagetable = kalloc_pagetable();

    void* allocated_page = kalloc(PAGESIZE);
    sys_page_free(allocated_page);
 
   // obtain reference to program image
   // (The program image models the process executable.)
   program_image pgm(program_name);
 
 
   for (vmiter p_it(kernel_pagetable, 0), c_it(ptable[pid].pagetable, 0);
       p_it.va() < MEMSIZE_VIRTUAL; p_it += PAGESIZE, c_it += PAGESIZE) {
       if (p_it.va() < PROC_START_ADDR) {
           // copy kernel and console mappings
           int f = c_it.try_map(p_it.pa(), p_it.perm());
           assert(f == 0); // mappings during kernel_start MUST NOT fail
       }
   }
 
   // allocate and map process memory as specified in program image
   for (auto seg = pgm.begin(); seg != pgm.end(); ++seg) {
       for (uintptr_t a = round_down(seg.va(), PAGESIZE);
           a < seg.va() + seg.size(); a += PAGESIZE) {
          
           // `a` is the process virtual address for the next code or data page
           uintptr_t new_pagep_addr = (uintptr_t) kalloc(PAGESIZE);
           vmiter p_it(ptable[pid].pagetable, a);
           if (seg.writable()) {
               p_it.map(new_pagep_addr, PTE_PWU);
           }
           else {
               // map the not writable pages as "not writable"
               p_it.map(new_pagep_addr, PTE_P | PTE_U);
           }
       }
   }
 
   // copy instructions and data from program image into process memory
   for (auto seg = pgm.begin(); seg != pgm.end(); ++seg) {
       vmiter p_it(ptable[pid].pagetable, seg.va());
       memset((void *) p_it.pa(), 0, seg.size());
       memcpy((void *) p_it.pa(), seg.data(), seg.data_size());
   }
 
   // mark entry point
   ptable[pid].regs.reg_rip = pgm.entry();
 
   // allocate and map stack segment
   // Compute process virtual address for stack page
   uintptr_t stack_addr = MEMSIZE_VIRTUAL - PAGESIZE;
   uintptr_t new_pagep_addr = (uintptr_t) kalloc(PAGESIZE);
   vmiter (ptable[pid].pagetable, stack_addr).map(new_pagep_addr, PTE_PWU);
 
   ptable[pid].regs.reg_rsp = stack_addr + PAGESIZE;
  
 
   // mark process as runnable
   ptable[pid].state = P_RUNNABLE;
}
 
 
 
// exception(regs)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `regs`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled when the kernel is running.
 
void exception(regstate* regs) {
   // Copy the saved registers into the `current` process descriptor.
   current->regs = *regs;
   regs = &current->regs;
 
   // It can be useful to log events using `log_printf`.
   // Events logged this way are stored in the host's `log.txt` file.
   /* log_printf("proc %d: exception %d at rip %p\n",
               current->pid, regs->reg_intno, regs->reg_rip); */
 
   // Show the current cursor location and memory state
   // (unless this is a kernel fault).
   console_show_cursor(cursorpos);
   if (regs->reg_intno != INT_PF || (regs->reg_errcode & PTE_U)) {
       memshow();
   }
 
   // If Control-C was typed, exit the virtual machine.
   check_keyboard();
 
 
   // Actually handle the exception.
   switch (regs->reg_intno) {
 
   case INT_IRQ + IRQ_TIMER:
       ++ticks;
       lapicstate::get().ack();
       schedule();
       break;                  /* will not be reached */
 
   case INT_PF: {
       // Analyze faulting address and access type.
       uintptr_t addr = rdcr2();
       const char* operation = regs->reg_errcode & PTE_W
               ? "write" : "read";
       const char* problem = regs->reg_errcode & PTE_P
               ? "protection problem" : "missing page";
 
       if (!(regs->reg_errcode & PTE_U)) {
           proc_panic(current, "Kernel page fault on %p (%s %s, rip=%p)!\n",
                      addr, operation, problem, regs->reg_rip);
       }
       error_printf(CPOS(24, 0), 0x0C00,
                    "Process %d page fault on %p (%s %s, rip=%p)!\n",
                    current->pid, addr, operation, problem, regs->reg_rip);
       current->state = P_FAULTED;
       break;
   }
 
   default:
       proc_panic(current, "Unhandled exception %d (rip=%p)!\n",
                  regs->reg_intno, regs->reg_rip);
 
   }
 
 
   // Return to the current process (or run something else).
   if (current->state == P_RUNNABLE) {
       run(current);
   } else {
       schedule();
   }
}
 
 
int syscall_page_alloc(uintptr_t addr);
int syscall_fork();
void syscall_exit();
 
// syscall(regs)
//    Handle a system call initiated by a `syscall` instruction.
//    The process’s register values at system call time are accessible in
//    `regs`.
//
//    If this function returns with value `V`, then the user process will
//    resume with `V` stored in `%rax` (so the system call effectively
//    returns `V`). Alternately, the kernel can exit this function by
//    calling `schedule()`, perhaps after storing the eventual system call
//    return value in `current->regs.reg_rax`.
//
//    It is only valid to return from this function if
//    `current->state == P_RUNNABLE`.
//
//    Note that hardware interrupts are disabled when the kernel is running.
 
uintptr_t syscall(regstate* regs) {
   // Copy the saved registers into the `current` process descriptor.
   current->regs = *regs;
   regs = &current->regs;
 
   // It can be useful to log events using `log_printf`.
   // Events logged this way are stored in the host's `log.txt` file.
   /* log_printf("proc %d: syscall %d at rip %p\n",
                 current->pid, regs->reg_rax, regs->reg_rip); */
 
   // Show the current cursor location and memory state.
   console_show_cursor(cursorpos);
   memshow();
 
   // If Control-C was typed, exit the virtual machine.
   check_keyboard();
 
 
   // Actually handle the exception.
   switch (regs->reg_rax) {
 
   case SYSCALL_PANIC:
       user_panic(current);
       break; // will not be reached
 
   case SYSCALL_GETPID:
       return current->pid;
 
   case SYSCALL_YIELD:
       current->regs.reg_rax = 0;
       schedule();             // does not return
 
   case SYSCALL_PAGE_ALLOC:
       return syscall_page_alloc(current->regs.reg_rdi);
 
   case SYSCALL_FORK:
       return syscall_fork();
  
   case SYSCALL_EXIT:
       syscall_exit();
       break;
    
    case SYSCALL_PAGE_FREE:
        sys_page_free((void*) current->regs.reg_rdi);
        return 0;

    case SYSCALL_KILL:
      return kill(regs->reg_rbx);

    case SYSCALL_SLEEP:
    // sys_sleep(ticks)
    //    Sleep for `ticks` timer interrupts.
    {
        ticks = regs->reg_rbx;
        if (ticks <= 0) {
        break;
        }

    // Decrement the sleep_ticks field of any sleeping processes
for (pid_t i = 0; i < NPROC; i++) {
  if (ptable[i].state == P_SLEEPING) {
    if (--ptable[i].sleep_ticks == 0) {
      // Wake up the process
      ptable[i].state = P_RUNNABLE;
      runqueue_add(&ptable[i]);
    }
  }
}


  }
  break;

   default:
       proc_panic(current, "Unhandled system call %ld (pid=%d, rip=%p)!\n",
                  regs->reg_rax, current->pid, regs->reg_rip);
 
   }
 
   panic("Should not get here!\n");
}
 
// free_proc(p)
// free all memory belonging to the process pointed to by p
 
void free_all(pid_t c_pid) {
   for (vmiter v_it(ptable[c_pid].pagetable, PROC_START_ADDR);
       v_it.va() < MEMSIZE_VIRTUAL; v_it += PAGESIZE) {
       if (v_it.perm()) {
           // free process memory
           kfree((void *) v_it.pa());
       }
   }
 
   // free the process pagetable
   for (ptiter p_it(ptable[c_pid].pagetable); p_it.va() < MEMSIZE_VIRTUAL; p_it.next()) {
       kfree((void *) p_it.pa());
   }
  
   // free the topmost pagetablepage
   kfree((void *) ptable[c_pid].pagetable);
   ptable[c_pid].pagetable = nullptr;
   ptable[c_pid].state = P_FREE;
  
}
 
// syscall_exit()
void syscall_exit() {
   free_all(current -> pid);
   schedule();
}
 
// syscall_fork()
int syscall_fork() {
   x86_64_pagetable* child_pt = kalloc_pagetable();
   if (current -> pagetable == nullptr || child_pt == nullptr) {
       return -1;
   }
   // find free slot in the ptable to insert child_pt 
   pid_t c_pid = -1;
   for (pid_t i = 1; i < NPROC; i++) {
       if (ptable[i].state == P_FREE) {
           ptable[i].pagetable = child_pt;
           c_pid = i;
           break;
       }
   }
   if (c_pid == -1) {
       // fail if there is no slot in ptable
       kfree(child_pt);
       return -1;
   } 
  
   for (vmiter p_it(current->pagetable, 0), c_it(child_pt, 0);
       p_it.va() < MEMSIZE_VIRTUAL; p_it += PAGESIZE, c_it += PAGESIZE) {
       if (p_it.va() < PROC_START_ADDR) {
           // copy kernel and console mappings to child (below the PROC_START_ADDR)
           int f = c_it.try_map(p_it.pa(), p_it.perm());
           if (f < 0) {
               // map fail
               free_all(c_pid);
               return -1;
           }
       }
       else if (p_it.perm() & PTE_P && p_it.perm() & PTE_U) {
           // allocate and map the process memory above
           uintptr_t current_page_addr;
           if (!p_it.writable()) {
               // share read only segments
               current_page_addr = p_it.pa();
               ++physpages[current_page_addr / PAGESIZE].refcount;
           }
           else {
               current_page_addr = (uintptr_t) kalloc(PAGESIZE);
               if (!current_page_addr) {
                   // kalloc fail
                   free_all(c_pid);
                   return -1;
               }
               // copy process memory
               memcpy((void *) current_page_addr, (void *) p_it.pa(), PAGESIZE);
           }
           int f = c_it.try_map(current_page_addr, p_it.perm());
           if (f < 0) {
               // proc map fail
               kfree((void*) current_page_addr);
               free_all(c_pid);
               return -1;
           }
       }
   } 
   // copy reg state
   ptable[c_pid].regs = current->regs;
   ptable[c_pid].regs.reg_rax = 0;
   ptable[c_pid].state = P_RUNNABLE;
   return c_pid;
}
 
 
// syscall_page_alloc(addr)
//    Handles the SYSCALL_PAGE_ALLOC system call. This function
//    should implement the specification for `sys_page_alloc`
//    in `u-lib.hh` (but in the handout code, it does not).
 
int syscall_page_alloc(uintptr_t addr) {
   // fails if the addr is not the process address
   if (addr < PROC_START_ADDR || addr % PAGESIZE != 0 || addr >= MEMSIZE_VIRTUAL) {
       return -1;
   }
 
   uintptr_t new_page_addr = (uintptr_t) kalloc(PAGESIZE);
   if (!new_page_addr) {
       // fail if out of physical memory
       return -1;
   }
  
   vmiter it(current->pagetable, addr);
   int f = it.try_map(new_page_addr, PTE_PWU);
   if (f < 0) {
       // free allocated page if mapping fails
       kfree((void *) new_page_addr);
       return -1;
   }
  
   memset((void*) it.pa(), 0, PAGESIZE);
   return 0;
  
}
 
 
// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.
 
void schedule() {
   pid_t pid = current->pid;
   for (unsigned spins = 1; true; ++spins) {
       pid = (pid + 1) % NPROC;
       if (ptable[pid].state == P_RUNNABLE) {
           run(&ptable[pid]);
       }
 
       // If Control-C was typed, exit the virtual machine.
       check_keyboard();
 
       // If spinning forever, show the memviewer.
       if (spins % (1 << 12) == 0) {
           memshow();
           log_printf("%u\n", spins);
       }
   }
}
 
 
// run(p)
//    Run process `p`. This involves setting `current = p` and calling
//    `exception_return` to restore its page table and registers.
 
void run(proc* p) {
   assert(p->state == P_RUNNABLE);
   current = p;
 
   // Check the process's current pagetable.
   check_pagetable(p->pagetable);
 
   // This function is defined in k-exception.S. It restores the process's
   // registers then jumps back to user mode.
   exception_return(p);
 
   // should never get here
   while (true) {
   }
}
 
 
// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.
 
void memshow() {
   static unsigned last_ticks = 0;
   static int showing = 0;
 
   // switch to a new process every 0.25 sec
   if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
       last_ticks = ticks;
       showing = (showing + 1) % NPROC;
   }
 
   proc* p = nullptr;
   for (int search = 0; !p && search < NPROC; ++search) {
       if (ptable[showing].state != P_FREE
           && ptable[showing].pagetable) {
           p = &ptable[showing];
       } else {
           showing = (showing + 1) % NPROC;
       }
   }
 
   console_memviewer(p);
   if (!p) {
       console_printf(CPOS(10, 26), 0x0F00, "   VIRTUAL ADDRESS SPACE\n"
           "                          [All processes have exited]\n"
           "\n\n\n\n\n\n\n\n\n\n\n");
   }
}
 

