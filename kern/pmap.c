/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/kclock.h>

// These variables are set by i386_mem_detect()
size_t npages;			// Amount of physical memory (in pages)
static size_t n_base_pages;	// Amount of base memory (in pages)

// These variables are set in mem_init()
pde_t *kern_pgdir;		// Kernel's initial page directory
struct Page *pages;		// Physical page state array

static Page *free_pages;	// Free list of physical pages

extern char bootstack[];	// Lowest addr in boot-time kernel stack

// Global descriptor table.
//
// The kernel and user segments are identical (except for the DPL).
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
struct Segdesc gdt[] = {
	SEG_NULL,				// 0x0 - unused (always faults)
	SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),	// 0x8 - kernel code segment
	SEG(STA_W, 0x0, 0xffffffff, 0),		// 0x10 - kernel data segment
	SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),	// 0x18 - user code segment
	SEG(STA_W, 0x0, 0xffffffff, 3),		// 0x20 - user data segment
	SEG_NULL				// 0x28 - tss, initialized in
						// idt_init()
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

static int
nvram_read(int r)
{
	return mc146818_read(r) | (mc146818_read(r + 1) << 8);
}

static void
i386_mem_detect(void)
{
	uint32_t n_extended_pages;
	
	// Use CMOS calls to measure available base & extended memory.
	// (CMOS calls return results in kilobytes.)
	n_base_pages = nvram_read(NVRAM_BASELO) * 1024 / PGSIZE;
	n_extended_pages = nvram_read(NVRAM_EXTLO) * 1024 / PGSIZE;

	// Calculate the maximum physical address based on whether
	// or not there is any extended memory.  See comment in <inc/mmu.h>.
	if (n_extended_pages)
		npages = (EXTPHYSMEM / PGSIZE) + n_extended_pages;
	else
		npages = n_base_pages;

	cprintf("Physical memory: %uK available, ", npages * PGSIZE / 1024);
	cprintf("base = %uK, extended = %uK\n", n_base_pages * PGSIZE / 1024,
		n_extended_pages * PGSIZE / 1024);
}


// --------------------------------------------------------------
// Set up initial memory mappings and turn on MMU.
// --------------------------------------------------------------

static void *boot_alloc(uint32_t n);
static void page_init(void);
static void page_map_segment(pte_t *pgdir, uintptr_t va, size_t size, physaddr_t pa, int perm);

static void page_alloc_check(void);
static void boot_mem_check(void);
static void page_check(void);


// Initializes virtual memory.
//
// Sets up the kernel's page directory 'kern_pgdir' (which contains those
// virtual memory mappings common to all user environments), installs that
// page directory, and turns on paging.  Then effectively turns off segments.
// 
// This function only sets up the kernel part of the address space
// (ie. addresses >= UTOP).  The user part of the address space
// will be set up later.
//
// From UTOP to ULIM, the user is allowed to read but not write.
// Above ULIM the user cannot read (or write). 
void
mem_init(void)
{
	uint32_t cr0;
	size_t n;
	int r;
	struct Page *pp;
	
	// Remove this line when you're ready to test this function.
	//panic("mem_init: This function is not finished\n");

	// Find out how much memory the machine has ('npages' & 'n_base_pages')
	i386_mem_detect();


	// Allocate 'pages', an array of 'struct Page' structures, one for
	// each physical memory page.  So there are 'npages' elements in the
	// array total (see i386_mem_detect()).
	// We advise you set the memory to 0 after allocating it, since that
	// will help you catch bugs later.
	//
	// LAB 2: Your code here.
	n = sizeof(struct Page) * npages;
	pages =(Page *)boot_alloc(n);

	// Now that we've allocated the 'pages' array, initialize it
	// by putting all free physical pages onto a list.  After this point,
	// all further memory management will go through the page_* functions.
	page_init();
	
	// Check your work so far.
	page_alloc_check();


	// Allocate the kernel's initial page directory, 'kern_pgdir'.
	// This starts out empty (all zeros).  Any virtual
	// address lookup using this empty 'kern_pgdir' would fault.
	// Then we add mappings to 'kern_pgdir' as we go long.
	pp = page_alloc();
	pp->pp_ref++;		// make sure we mark the page as used!

	kern_pgdir = (pte_t *) pp->data();
	memset(kern_pgdir, 0, PGSIZE);

	// Check page mapping functions.
	page_check();


	// Map the kernel stack at virtual address 'KSTACKTOP-KSTKSIZE'.
	// A large range of virtual memory, [KSTACKTOP-PTSIZE, KSTACKTOP),
	// is marked out for the kernel stack.  However, only part of this
	// range is allocated.  The rest of it is left unmapped, so that
	// kernel stack overflow will cause a page fault.
	// - [KSTACKTOP-PTSIZE, KSTACKTOP-KSTKSIZE) -- not present
	// - [KSTACKTOP-KSTKSIZE, KSTACKTOP) -- kernel RW, user NONE
	//
	// The kernel already has a stack, so it is not necessary to allocate
	// a new one.  That stack's bottom address is 'bootstack'.  (Q: Where
	// is 'bootstack' allocated?)
	//
	// LAB 2: Your code here.
        //page_map_segment(kern_pgdir, KSTACKTOP - PTSIZE, PTSIZE - KSTKSIZE, 0x0, 0);
        page_map_segment(kern_pgdir, KSTACKTOP - KSTKSIZE, KSTKSIZE, PADDR(bootstack), PTE_P | PTE_W);
	// Map all of physical memory at KERNBASE.
	// I.e., the VA range [KERNBASE, 2^32) should map to
	//       the PA range [0, 2^32 - KERNBASE).
	// We might not have 2^32 - KERNBASE bytes of physical memory, but
	// we just set up the mapping anyway.
	// Permissions: kernel RW, user NONE
	//
 	// LAB 2: Your code here.
        page_map_segment(kern_pgdir, KERNBASE, ~KERNBASE + 1, 0x0, PTE_P | PTE_W);
	// Check that the initial page directory has been set up correctly.
	boot_mem_check();

	// On x86, segmentation maps a VA to a LA (linear addr) and
	// paging maps the LA to a PA; we write VA => LA => PA.  If paging is
	// turned off the LA is used as the PA.  There is no way to
	// turn off segmentation; the closest thing is to set the base
	// address to 0, so the VA => LA mapping is the identity.

	// The current mapping: VA KERNBASE+x => PA x.
	//     (segmentation base = -KERNBASE, and paging is off.)

	// From here on down we must maintain this VA KERNBASE + x => PA x
	// mapping, even though we are turning on paging and reconfiguring
	// segmentation.

	// Map VA 0:4MB same as VA KERNBASE, i.e. to PA 0:4MB.
	// (Limits our kernel to <4MB)
	kern_pgdir[0] = kern_pgdir[PDX(KERNBASE)];

	// Install page table.
	lcr3(PADDR(kern_pgdir));

	// Turn on paging.
	cr0 = rcr0();
	cr0 |= CR0_PE|CR0_PG|CR0_AM|CR0_WP|CR0_NE|CR0_TS|CR0_EM|CR0_MP;
	cr0 &= ~(CR0_TS|CR0_EM);
	lcr0(cr0);

	// Current mapping: VA KERNBASE+x => LA x => PA x.
	// (x < 4MB so uses paging kern_pgdir[0])

	// Reload all segment registers.
	asm volatile("lgdt gdt_pd");
	asm volatile("movw %%ax,%%gs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%es" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" :: "a" (GD_KD));
	asm volatile("ljmp %0,$1f\n 1:\n" :: "i" (GD_KT));  // reload cs
	asm volatile("lldt %%ax" :: "a" (0));

	// Final mapping: VA KERNBASE+x => LA KERNBASE+x => PA x.

	// This mapping was only used after paging was turned on but
	// before the segment registers were reloaded.
	kern_pgdir[0] = 0;

	// Flush the TLB for good measure, to kill the kern_pgdir[0] mapping.
	lcr3(PADDR(kern_pgdir));
}


// This simple physical memory allocator is used only while JOS is setting
// up its virtual memory system.  page_alloc() is the real allocator.
//
// Allocate enough pages of contiguous physical memory to hold 'n' bytes.
// Doesn't initialize the memory.  Returns a kernel virtual address.
//
// If 'n' is 0, boot_alloc() should return the KVA of the next free page
// (without allocating anything).
//
// If we're out of memory, boot_alloc should panic.
// This function may ONLY be used during initialization,
// before the free_pages list has been set up.
static void *
boot_alloc(uint32_t n)
{
	extern char end[];
	static char *nextfree;	// pointer to next byte of free mem
	void *v;

	// Initialize nextfree if this is the first time.
	// 'end' is a magic symbol automatically generated by the linker,
	// which points to the end of the kernel's bss segment:
	// the first virtual address that the linker did *not* assign
	// to any kernel code or global variables.
	if (nextfree == 0)
		nextfree = round_up((char *) end, PGSIZE);

	// Allocate a chunk large enough to hold 'n' bytes, then update
	// nextfree.  Make sure nextfree is kept aligned
	// to a multiple of PGSIZE.
	//
	// LAB 2: Your code here.
	// Hole 'n' bytes memory
	v = (void *) nextfree;
	nextfree += n;
	// Align nextfree
	nextfree = round_up (nextfree, PGSIZE);
	// Panic when out of memory
	if((unsigned int) nextfree < (unsigned int) end)
	panic("boot_alloc: out of memory.\n");
	return v;
}


// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pgdir'.  The hardware normally performs
// this functionality for us!  We define our own version to help
// the boot_mem_check() function; it shouldn't be used elsewhere.
static physaddr_t
check_va2pa(pde_t *pgdir, uintptr_t va)
{
	pte_t *p;

	pgdir = &pgdir[PDX(va)];
	if (!(*pgdir & PTE_P))
		return ~0;
	p = (pte_t*) KADDR(PTE_ADDR(*pgdir));
	if (!(p[PTX(va)] & PTE_P))
		return ~0;
	return PTE_ADDR(p[PTX(va)]);
}
		

// --------------------------------------------------------------
// Tracking of physical pages.
// The 'pages' array has one 'struct Page' entry per physical page.
// Pages are reference counted, and free pages are kept on a linked list.
// --------------------------------------------------------------

// Initialize page structure and memory free list.
// After this point, ONLY use the page_ functions
// to allocate and deallocate physical memory via the free_pages list,
// and NEVER use boot_alloc() or the related boot-time functions above.
void
page_init(void)
{
	// The example code here marks all pages as free.
	// However this is not truly the case.  What memory is free?
	//  1) Mark page 0 as in use.
	//     This way we preserve the real-mode IDT and BIOS structures
	//     in case we ever need them.  (Currently we don't, but...)
	//  2) Mark the rest of base memory as free.
	//  3) Then comes the IO hole [IOPHYSMEM, EXTPHYSMEM).
	//     Mark it as in use so that it can never be allocated.      
	//  4) Then extended memory [EXTPHYSMEM, ...).
	//     Some of it is in use, some is free.  Where is the kernel?
	//     Which pages are used for page tables and other data structures
	//     allocated by boot_alloc()?  (Hint: boot_alloc() will tell you
	//     if you give it the right argument!)
	//
	// Change the code to reflect this.
	//struct Page *page_begin, *page_end;
	//page_begin = &pages[IOPHYSMEM/PGSIZE];
	//page_end = boot_alloc(0);
	unsigned int pbegin, pend;
	//page's number which map IOPHYSMEM 
	pbegin = PGNUM(IOPHYSMEM);
	//last page had been allocated in extended memory
	pend = PGNUM(PADDR(boot_alloc(0))) + 1;
	for (size_t i = 0; i < npages; i++)
	{
	        // Initialize the page structure
		// Mark page0 in use
		if(i == 0)
		{
		pages[i].pp_ref = 1;
		continue;
		}
		//Mark IO hole and pages allocate by boot_alloc(0);
		if(i >= pbegin && i < pend)
		{
		pages[i].pp_ref = 1;
		continue;
		}
		pages[i].pp_ref = 0;
		//Add the free pages into the link free_pages
		pages[i].pp_next = free_pages;
		free_pages = &pages[i];
	}
}

// Allocate a physical page, without necessarily initializing it.
// Returns a pointer to the Page struct of the newly allocated page.
// If there were no free pages, returns NULL.
//
// Hint: set the Page's fields appropriately
// Hint: the returned page's pp_ref should be zero
// Software Engineering Hint: It can be extremely useful for later debugging
//   if you erase allocated memory.  For instance, you might write the value
//   0xCC (the int3 instruction) over the page before you return it.  This will
//   cause your kernel to crash QUICKLY if you ever make a bookkeeping mistake,
//   such as freeing a page while someone is still using it.  A quick crash is
//   much preferable to a SLOW crash, where *maybe* a long time after your
//   kernel boots, a data structure gets corrupted because its containing page
//   was used twice!  Note that erasing the page with a non-zero value is
//   usually better than erasing it with 0.  (Why might this be?)
struct Page *
page_alloc()
{
	// Fill this function in
	struct Page * alloc_page;
	if (free_pages == NULL)
	return NULL;
	// Get a free page from link free_pages, but no initial
	alloc_page = free_pages;
	// Delete allocted page from link free_pages
	free_pages = free_pages->pp_next;
	alloc_page->pp_next = NULL;
	// Erase allocted memory
	memset(alloc_page->data(), 0xCC, sizeof(*alloc_page));
	return alloc_page;
}

// Return a page to the free list.
// (This function should only be called when pp->pp_ref reaches 0:
// you may want to check this with an assert.)
//
// Software Engineering Hint: It can be extremely useful for later debugging
//   if you erase each page's memory as soon as it is freed.  See the Software
//   Engineering Hint above for reasons why.
void
page_free(struct Page *pp)
{
	// Fill this function in
	//Only be called when pp_ref reaches 0
	assert(pp->pp_ref == 0);
	//Add free page to link free_pages
	pp->pp_next = free_pages;
	free_pages = pp;
	//erase page memory
	memset(pp->data(), 0xCD, sizeof(*pp));
}

// Decrement the reference count on a page.
// Free it if there are no more refs afterwards.
void
page_decref(struct Page *pp)
{
	if (--pp->pp_ref == 0)
		page_free(pp);
}

// Given 'pgdir', a pointer to a page directory, pgdir_walk returns
// a pointer to the page table entry (PTE) for linear address 'va'.
// This requires walking the two-level page table structure.
//
// If the relevant page table doesn't exist in the page directory, then:
//    - If create == 0, pgdir_walk returns NULL.
//    - Otherwise, pgdir_walk tries to allocate a new page table
//	with page_alloc.  If this fails, pgdir_walk returns NULL.
//    - Otherwise, pgdir_walk returns a pointer into the new page table.
//
// Hint: you can turn a Page * into the physical address of the
// page it refers to with Page::physaddr() from inc/memlayout.h.
pte_t *
pgdir_walk(pde_t *pgdir, uintptr_t va, int create)
{
	// Fill this function in
	struct Page *newpage;
	// If find, return the pointer to the page table entry(PTE)
	if((pgdir[PDX(va)] & PTE_P) != 0)
	return &(((pte_t *)KADDR(PTE_ADDR(pgdir[PDX(va)])))[PTX(va)]); 
	// If create == 0, pgdir_walk returns NULL
	else if(create == 0)
		return NULL;
	     else
		{
	// Allocate a new page table
		newpage = page_alloc();
		if(!newpage)
			return NULL;
		newpage->pp_ref++;
		memset(newpage->data(), 0, PGSIZE);
		pgdir[PDX(va)] = newpage->physaddr() | PTE_P | PTE_U | PTE_W;
	// Return the new page entry pointer
		return &(((pte_t *)KADDR(PTE_ADDR(pgdir[PDX(va)])))[PTX(va)]);
		}	
	return NULL;
}

// Maps the physical page 'pp' at virtual address 'va'.
// The permissions (the low 12 bits) of the page table entry
// are set to 'perm|PTE_P'.
//
// Details
//   - If there is already a page mapped at 'va', it is page_remove()d.
//   - If necessary, on demand, allocates a page table and inserts it into
//     'pgdir'.
//   - pp->pp_ref should be incremented if the insertion succeeds.
//   - The TLB must be invalidated if a page was formerly present at 'va'.
//   - It is safe to page_insert() a page that is already mapped at 'va'.
//     This is useful to change permissions for a page.
//
// RETURNS: 
//   0 on success
//   -E_NO_MEM, if page table couldn't be allocated
//
// Hint: The TA solution is implemented using pgdir_walk, page_remove,
// and Page::physaddr.
int
page_insert(pde_t *pgdir, struct Page *pp, uintptr_t va, int perm) 
{
	// Fill this function in
	pte_t *ppte = pgdir_walk(pgdir, va, 1);
	// Allocate page failed
	if(ppte == NULL)
		return -E_NO_MEM;
	if(((*ppte) & PTE_P) != 0)
	{
		// Already page mapped, need be removed
		if(PTE_ADDR(*ppte) != pp->physaddr())
		page_remove(pgdir, va);
		else
		pp->pp_ref--;
	}
	// Set permissions
	*ppte = pp->physaddr() | perm | PTE_P;
	pp->pp_ref++;
	// invalidate TLB of former page at 'va'
	tlb_invalidate(pgdir, va);
	return 0;
}

// Returns the page mapped at virtual address 'va'.
// If pte_store is not null, then '*pte_store' is set to the address
// of the pte for this page.  This is used by page_remove.
//
// Returns 0 if there is no page mapped at va.
//
// Hint: the TA solution uses pgdir_walk.
struct Page *
page_lookup(pde_t *pgdir, uintptr_t va, pte_t **pte_store)
{
	// Fill this function in
	// Lookup virtual address 'va' in page direction
	pte_t *ppte = pgdir_walk(pgdir, va, 0);
	if(ppte == NULL)
		return NULL;
	// Set '*pte_store' to the address of the pte
	if(pte_store != NULL)
		(*pte_store) = ppte;
	// Return the mapped page address
	if(*ppte & PTE_P)
		return &pages[PGNUM(*ppte)];
	return NULL;
}

// Unmaps the physical page at virtual address 'va'.
// If there is no physical page at that address, silently does nothing.
//
// Details:
//   - The ref count on the physical page should decrement.
//   - The physical page should be freed if the refcount reaches 0.
//   - The page table entry corresponding to 'va' should be set to 0
//     (if such a PTE exists).
//   - The TLB must be invalidated if you remove an entry from
//     the pg dir/pg table.
//
// Hint: The TA solution is implemented using page_lookup,
// 	tlb_invalidate, and page_decref.
void
page_remove(pde_t *pgdir, uintptr_t va)
{
	// Fill this function in
	pte_t *ppte;
	// Lookup 'va' to identify if any physical page at that address
	struct Page *page = page_lookup(pgdir, va, &ppte);
	// If no physical page mapped just return
	if(page == NULL)
		return;
	// Else decrement the ref count
	page_decref(page);
	// Set corresponding page table entry (PTE) to 0
	*ppte = 0;
	// Invalidated the TLB
	tlb_invalidate(pgdir, va);
	return;
}

// Invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
void
tlb_invalidate(pde_t *pgdir, uintptr_t va)
{
	// Flush the entry only if we're modifying the current address space.
	// For now, there is only one address space, so always invalidate.
	invlpg(va);
}


// Map [la, la+size) of linear address space to physical [pa, pa+size)
// in the kernel's page table 'kern_pgdir'.  Size is a multiple of PGSIZE.
// Use permission bits perm|PTE_P for the entries.
//
// This function resembles page_insert(), but is meant for use at boot time on
// reserved portions of physical memory.
// There's no need here to manage page reference counts or invalidate the TLB.
static void
page_map_segment(pte_t *pgdir, uintptr_t la, size_t size, physaddr_t pa,
		 int perm)
{
	// LAB 2: Your code here.
	size_t index;
	pte_t *ppte;
	assert((la & (PGSIZE - 1)) == 0 );
	assert((pa & (PGSIZE - 1)) == 0 );
	// Ensure size is a multiple of PGSIZE
	size = round_up(size, PGSIZE);
	// Map linear address to physical
	for(index = 0; index < size; index += PGSIZE)
	{
		ppte = pgdir_walk(pgdir, (la + index), 1);
		assert(ppte != NULL);
		// Set permission of PTE
		(*ppte) = (pa + index) | perm | PTE_P;
	}
	return;
}


// These functions check your work.

// Check the physical page allocator (page_alloc(), page_free(),
// and page_init()).
static void
page_alloc_check()
{
	struct Page *pp, *pp0, *pp1, *pp2, *fl;
	
        // if there's a page that shouldn't be on
        // the free list, try to make sure it
        // eventually causes trouble.
	for (pp0 = free_pages; pp0; pp0 = pp0->pp_next)
		memset(pp0->data(), 0x97, 128);

	// should be able to allocate three pages
	pp0 = page_alloc();
	pp1 = page_alloc();
	pp2 = page_alloc();
	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);
        assert(pp0->page_number() < npages);
        assert(pp1->page_number() < npages);
        assert(pp2->page_number() < npages);

	// temporarily steal the rest of the free pages
	fl = free_pages;
	free_pages = NULL;

	// should be no free memory
	pp = page_alloc();
	assert(pp == NULL);

        // free and re-allocate?
        page_free(pp0);
        page_free(pp1);
        page_free(pp2);
	pp0 = page_alloc();
	pp1 = page_alloc();
	pp2 = page_alloc();
	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);
	pp = page_alloc();
	assert(pp == NULL);

	// give free list back
	free_pages = fl;

	// free the pages we took
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);

	cprintf("page_alloc_check() succeeded!\n");
}

// Checks that the kernel part of virtual address space
// has been set up roughly correctly (by mem_init()).
//
// This function doesn't test every corner case,
// and doesn't test the permission bits at all,
// but it is a pretty good sanity check.
static void
boot_mem_check(void)
{
	uint32_t i, n;

	// check phys mem
	for (i = 0; KERNBASE + i != 0; i += PGSIZE)
		assert(check_va2pa(kern_pgdir, KERNBASE + i) == i);

	// check kernel stack
	for (i = 0; i < KSTKSIZE; i += PGSIZE)
		assert(check_va2pa(kern_pgdir, KSTACKTOP - KSTKSIZE + i) == PADDR(bootstack) + i);

	// check for zero/non-zero in PDEs
	for (i = 0; i < NPDENTRIES; i++) {
		switch (i) {
		case PDX(KSTACKTOP-1):
			assert(kern_pgdir[i]);
			break;
		default:
			if (i >= PDX(KERNBASE))
				assert(kern_pgdir[i]);
			else
				assert(kern_pgdir[i] == 0);
			break;
		}
	}
	
	cprintf("boot_mem_check() succeeded!\n");
}

void
page_check(void)
{
	struct Page *pp, *pp0, *pp1, *pp2, *fl;
	pte_t *ptep, *ptep1;
	uintptr_t va;

	// should be able to allocate three pages
	pp0 = page_alloc();
	pp1 = page_alloc();
	pp2 = page_alloc();
	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);

	// temporarily steal the rest of the free pages
	fl = free_pages;
	free_pages = NULL;

	// should be no free memory
	pp = page_alloc();
	assert(pp == NULL);

	// there is no page allocated at address 0
	assert(page_lookup(kern_pgdir, 0, &ptep) == NULL);

	// there is no free memory, so we can't allocate a page table 
	assert(page_insert(kern_pgdir, pp1, 0, 0) < 0);

	// free pp0 and try again: pp0 should be used for page table
	page_free(pp0);
	assert(page_insert(kern_pgdir, pp1, 0, 0) == 0);
	assert(PTE_ADDR(kern_pgdir[0]) == pp0->physaddr());
	assert(check_va2pa(kern_pgdir, 0) == pp1->physaddr());
	assert(pp1->pp_ref == 1);
	assert(pp0->pp_ref == 1);

	// should be able to map pp2 at PGSIZE because pp0
	// is already allocated for page table
	assert(page_insert(kern_pgdir, pp2, PGSIZE, 0) == 0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == pp2->physaddr());
	assert(pp2->pp_ref == 1);

	// should be no free memory
	pp = page_alloc();
	assert(pp == NULL);

	// should be able to map pp2 at PGSIZE because it's already there
	assert(page_insert(kern_pgdir, pp2, PGSIZE, 0) == 0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == pp2->physaddr());
	assert(pp2->pp_ref == 1);

	// pp2 should NOT be on the free list
	// could happen in ref counts are handled sloppily in page_insert
	pp = page_alloc();
	assert(pp == NULL);

	// check that pgdir_walk returns a pointer to the pte
	ptep = (pte_t *) KADDR(PTE_ADDR(kern_pgdir[PDX(PGSIZE)]));
	assert(pgdir_walk(kern_pgdir, PGSIZE, 0) == ptep + PTX(PGSIZE));

	// should be able to change permissions too.
	assert(page_insert(kern_pgdir, pp2, PGSIZE, PTE_U) == 0);
	assert(check_va2pa(kern_pgdir, PGSIZE) == pp2->physaddr());
	assert(pp2->pp_ref == 1);
	assert(*pgdir_walk(kern_pgdir, PGSIZE, 0) & PTE_U);
	
	// should not be able to map at PTSIZE because need free page
	// for page table
	assert(page_insert(kern_pgdir, pp0, PTSIZE, 0) < 0);

	// insert pp1 at PGSIZE (replacing pp2)
	assert(page_insert(kern_pgdir, pp1, PGSIZE, 0) == 0);

	// should have pp1 at both 0 and PGSIZE, pp2 nowhere, ...
	assert(check_va2pa(kern_pgdir, 0) == pp1->physaddr());
	assert(check_va2pa(kern_pgdir, PGSIZE) == pp1->physaddr());
	// ... and ref counts should reflect this
	assert(pp1->pp_ref == 2);
	assert(pp2->pp_ref == 0);

	// pp2 should be returned by page_alloc
	pp = page_alloc();
	assert(pp == pp2);

	// unmapping pp1 at 0 should keep pp1 at PGSIZE
	page_remove(kern_pgdir, 0);
	assert(check_va2pa(kern_pgdir, 0) == ~0U);
	assert(check_va2pa(kern_pgdir, PGSIZE) == pp1->physaddr());
	assert(pp1->pp_ref == 1);
	assert(pp2->pp_ref == 0);

	// unmapping pp1 at PGSIZE should free it
	page_remove(kern_pgdir, PGSIZE);
	assert(check_va2pa(kern_pgdir, 0) == ~0U);
	assert(check_va2pa(kern_pgdir, PGSIZE) == ~0U);
	assert(pp1->pp_ref == 0);
	assert(pp2->pp_ref == 0);

	// so it should be returned by page_alloc
	pp = page_alloc();
	assert(pp == pp1);

	// should be no free memory
	pp = page_alloc();
	assert(pp == NULL);

#if 0
	// should be able to page_insert to change a page
	// and see the new data immediately.
	memset(pp1->data(), 1, PGSIZE);
	memset(pp2->data(), 2, PGSIZE);
	page_insert(kern_pgdir, pp1, 0, 0);
	assert(pp1->pp_ref == 1);
	assert(pp2->pp_ref == 0);
#endif

	// forcibly take pp0 back
	assert(PTE_ADDR(kern_pgdir[0]) == pp0->physaddr());
	kern_pgdir[0] = 0;
	assert(pp0->pp_ref == 1);
	pp0->pp_ref = 0;

	// check pointer arithmetic in pgdir_walk
	page_free(pp0);
	va = PTSIZE + PGSIZE;
	ptep = pgdir_walk(kern_pgdir, va, 1);
	ptep1 = (pte_t *) KADDR(PTE_ADDR(kern_pgdir[PDX(va)]));
	assert(ptep == ptep1 + PTX(va));
	kern_pgdir[PDX(va)] = 0;
	pp0->pp_ref = 0;
	
	// check that new page tables get cleared
	memset(pp0->data(), 0xFF, PGSIZE);
	page_free(pp0);
	pgdir_walk(kern_pgdir, 0x0, 1);
	ptep = (pte_t *) pp0->data();
	for (size_t i = 0; i < NPTENTRIES; i++)
		assert((ptep[i] & PTE_P) == 0);
	kern_pgdir[0] = 0;
	pp0->pp_ref = 0;

	// give free list back
	free_pages = fl;

	// free the pages we took
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);
	
	cprintf("page_check() succeeded!\n");
}

