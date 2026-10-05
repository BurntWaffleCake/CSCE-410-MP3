#include "assert.H"
#include "console.H"
#include "exceptions.H"
#include "page_table.H"
#include "paging_low.H"

#define PG_BIT 0x80000000  // bit 31

// bytes of virtual memory mapped by one page table (4 MB)
#define PAGE_TABLE_SPAN 0x400000

#define PRESENT_BIT 0x1
#define WRITABLE_BIT 0x2
#define USER_BIT 0x4

PageTable* PageTable::current_page_table = nullptr;
unsigned int PageTable::paging_enabled = 0;
ContFramePool* PageTable::kernel_mem_pool = nullptr;
ContFramePool* PageTable::process_mem_pool = nullptr;
unsigned long PageTable::shared_size = 0;

void PageTable::init_paging(ContFramePool* _kernel_mem_pool,
                            ContFramePool* _process_mem_pool,
                            const unsigned long _shared_size) {
  // sanity checks
  // ensure passed frame pools are not null
  assert(_kernel_mem_pool != nullptr);
  assert(_process_mem_pool != nullptr);
  // ensure shared size is positive and is a multiple of page table span
  assert(_shared_size > 0);
  assert(_shared_size % PAGE_TABLE_SPAN == 0);

  // assign static members
  PageTable::kernel_mem_pool = _kernel_mem_pool;
  PageTable::process_mem_pool = _process_mem_pool;
  PageTable::shared_size = _shared_size;

  Console::puts("Initialized Paging System\n");
}

// build an address space with shared region mapped and everything else not
// present the constructor does not load the page table use load() to set the
// current page table
PageTable::PageTable() {
  // allocate a frame for the page directory
  unsigned long frame = PageTable::kernel_mem_pool->get_frames(1);
  // physical address of page directory
  this->page_directory = (unsigned long*)(frame * PAGE_SIZE);

  // total number of page tables needed to cover the shared address space
  unsigned long num_page_tables = PageTable::shared_size / PAGE_TABLE_SPAN;

  // initialize the page directory with page tables needed by the shared address
  // space
  for (unsigned long t = 0; t < num_page_tables; t++) {
    unsigned long page_table_frame = PageTable::kernel_mem_pool->get_frames(1);
    // physical address of page table
    unsigned long* page_table = (unsigned long*)(page_table_frame * PAGE_SIZE);

    // mark all entries as present and writable
    // each page maps to same physical address
    for (unsigned long i = 0; i < ENTRIES_PER_PAGE; i++) {
      page_table[i] =
          ((t * ENTRIES_PER_PAGE + i) * PAGE_SIZE) | PRESENT_BIT | WRITABLE_BIT;
    }

    // mark page directory present and writable
    this->page_directory[t] =
        (page_table_frame * PAGE_SIZE) | (PRESENT_BIT | WRITABLE_BIT);
  }

  // mark remaining page directory entries as writable but not present
  for (unsigned long t = num_page_tables; t < ENTRIES_PER_PAGE; t++) {
    this->page_directory[t] = WRITABLE_BIT;
  }

  Console::puts("Constructed Page Table object\n");
}

// set active page table to this one
void PageTable::load() {
  // write the address of the page directory to CR3
  write_cr3((unsigned long)this->page_directory);
  PageTable::current_page_table = this;
  Console::puts("Loaded page table\n");
}

// turn translation on
void PageTable::enable_paging() {
  // Set the PG bit (bit 31) in CR0
  // CR3 must already point to a valid page directory (see load())
  // otherwise the next fetch will fault
  write_cr0(read_cr0() | PG_BIT);
  PageTable::paging_enabled = 1;
  Console::puts("Enabled paging\n");
}

void PageTable::handle_fault(REGS* _r) {
  // read the faulting address from CR2
  unsigned long faulting_address = read_cr2();

  // page is present but access violates protection
  if (_r->err_code & 0x1) {
    Console::puts("Page fault: protection violation\n");
    assert(false);
  }

  // physical address of page directory
  // first 4MB is mapped one-to-one
  unsigned long* page_directory = PageTable::current_page_table->page_directory;

  // destructure the faulting address into directory and table indices
  unsigned long directory_index = (faulting_address >> 22);
  // shift off the offset, keep the next 10 bits
  unsigned long table_index = (faulting_address >> 12) & 0x3FF;

  // check if the page table is present and allocate if not
  if (!(page_directory[directory_index] & PRESENT_BIT)) {
    // allocate a new frame for a new page table
    unsigned long frame = PageTable::kernel_mem_pool->get_frames(1);
    page_directory[directory_index] =
        (frame * PAGE_SIZE) | PRESENT_BIT | WRITABLE_BIT;

    unsigned long* new_table = (unsigned long*)(frame * PAGE_SIZE);

    // mark all entries in the new page table as writable but not present
    for (unsigned long i = 0; i < ENTRIES_PER_PAGE; i++) {
      new_table[i] = WRITABLE_BIT;
    }
  }

  // obtain page table address with flags zeroed out
  // otherwise the page_table address would point beyond the flags and be offset
  unsigned long* page_table =
      (unsigned long*)(page_directory[directory_index] & 0xFFFFF000);

  // allocate new process frame
  unsigned long data_frame = PageTable::process_mem_pool->get_frames(1);

  // update the page table with the address of the new process frame
  page_table[table_index] =
      (data_frame * PAGE_SIZE) | PRESENT_BIT | WRITABLE_BIT;
}