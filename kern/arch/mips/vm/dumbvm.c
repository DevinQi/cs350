/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <syscall.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
static uint32_t *frame_size;
static uint32_t *frame_use;
static paddr_t mem_start;
static paddr_t mem_end;
static int frame_max;
static int frame_first_free;
static bool vm_ready = false;
#endif // OPT_A3

void
vm_bootstrap(void)
{
#if OPT_A3
	//Grab all memory from ram
	ram_getsize(&mem_start, &mem_end);

	//Max mem is less than 512MB = 131072 frames
	//131072 frames at 1 bit each = 16384 bytes = 4 frames

	//Dedicate 4 frames to hold frame_use bitfield
	frame_use = (uint32_t *) PADDR_TO_KVADDR(mem_start);
	mem_start += sizeof(uint32_t) * PAGE_SIZE;

	//Calculate number of frames we can fit into
	//Each page takes PAGE_SIZE + 4 bytes(frame-size)
	frame_max = (mem_end - mem_start) / (PAGE_SIZE + 4);

	//Dedicate an array of integers to save continuous frame sizes
	mem_end -= sizeof(uint32_t) * frame_max;
	frame_size = (uint32_t *) PADDR_TO_KVADDR(mem_end);

	//Mark first free frame
	frame_first_free = 0;

	//Clear bitfield and size arrays
	for(int i = 0; i < frame_max/32; i++) {
		frame_use[i] = 0;
	}
	for(int i = 0; i < frame_max; i++) {
		frame_size[i] = 0;
	}

	kprintf("vm: %d frames available \n", frame_max);
	vm_ready = true;
#else
	/* Do nothing. */
#endif // OPT_A3
}

#if OPT_A3

static
bool
frame_get_use(int frame_number)
{
	KASSERT(frame_number < frame_max);
	int index = frame_number / 32;
	uint32_t mask = ((uint32_t)1) << (frame_number%32);
	return (frame_use[index] & mask) != 0;
}

static
void
frame_set_use(int frame_number, bool use)
{
	KASSERT(frame_number < frame_max);
	int index = frame_number / 32;
	uint32_t mask = ((uint32_t)1) << (frame_number%32);

	if(use) {
		frame_use[index] |= mask;
	}
	else {
		frame_use[index] &= ~mask;
	}
}

static
paddr_t
alloc_frames(int npages)
{
	int frame_number;

	spinlock_acquire(&stealmem_lock);

	//Start with first free
	frame_number = frame_first_free;

	//Look until we find a first spot
	while(frame_number < frame_max) {
		if(!frame_get_use(frame_number)) {
			//Found a free spot
			//Check if there is enough continuous space
			bool enough_frames = true;
			for(int i = 1; i < npages; i++) {
				if(frame_get_use(frame_number + i)) {
					enough_frames = false;
					//Advance our frame number to the next unchecked frame
					frame_number += i + 1;
					break;
				}
			}
			//Enough space found. Use it!
			if(enough_frames) {
				//First mark all the frames used
				for(int i = 0; i < npages; i++) {
					frame_set_use(frame_number + i, true);
					frame_size[frame_number + i] = 0;
				}
				//Mark the size of the frames
				frame_size[frame_number] = npages;
				//Update the first frame if necessary
				if(frame_number == frame_first_free) {
					for(int i = frame_number + 1; i < frame_max; i++) {
						if(!frame_get_use(i)) {
							frame_first_free = i;
							break;
						}
					}
				}
				spinlock_release(&stealmem_lock);
				return mem_start + (frame_number * PAGE_SIZE);
			}
		}
		else {
			frame_number++;
		}
	}

	//Arriving here means no space available
	spinlock_release(&stealmem_lock);
	return 0;
}

static
void
free_frames(paddr_t frame)
{
	int frame_number = (frame - mem_start) / PAGE_SIZE;
	int npages;

	KASSERT((frame - mem_start) % PAGE_SIZE == 0);
	KASSERT(frame_number >= 0);
	KASSERT(frame_number < frame_max);
	KASSERT(frame_get_use(frame_number));

	spinlock_acquire(&stealmem_lock);

	npages = frame_size[frame_number];

	//Mark all the frames as free
	for(int i = 0; i < npages; i++) {
		frame_set_use(frame_number + i, false);
		frame_size[frame_number + i] = 0;
	}

	//Update the first frame if necessary
	if(frame_number < frame_first_free) {
		frame_first_free = frame_number;
	}

	spinlock_release(&stealmem_lock);
}

static
void
free_multi_frames(paddr_t * pframes, int npages)
{
	for(int i = 0; i < npages; i++) {
		free_frames(pframes[i]);
	}
	kfree(pframes);
}

//WARNING: UNSAFE
//Uses kmalloc, and thus may cause infinite recursion if inappropriately used
static
paddr_t *
alloc_multi_frames(int npages)
{
	paddr_t *result;

	result = kmalloc(npages * sizeof(paddr_t *));
	if(result == NULL) {
		return 0;
	}

	for(int i = 0; i < npages; i++) {
		result[i] = alloc_frames(1);
		if(result[i] == 0) {
			free_multi_frames(result, i);
			return 0;
		}
	}

	return result;
}

#endif // OPT_A3

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
#if OPT_A3
	if(vm_ready) {
		pa = alloc_frames(npages);
	}
	else {
		pa = getppages(npages);
	}
#else
	pa = getppages(npages);
#endif // OPT_A3
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
#if OPT_A3
	free_frames(KVADDR_TO_PADDR(addr));
#else
	/* nothing - leak the memory. */

	(void)addr;
#endif // OPT_A3
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
#if OPT_A3
	uint32_t rw;
#endif // OPT_A3

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
#if OPT_A3
		return EFAULT;
#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
#endif // OPT_A3
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
#if OPT_A3
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != NULL);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != NULL);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != NULL);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	for(int i = 0; i < (int)as->as_npages1; i++) {
		KASSERT((as->as_pbase1[i] & PAGE_FRAME) == as->as_pbase1[i]);
	}
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	for(int i = 0; i < (int)as->as_npages2; i++) {
		KASSERT((as->as_pbase2[i] & PAGE_FRAME) == as->as_pbase2[i]);
	}
	for(int i = 0; i < DUMBVM_STACKPAGES; i++) {
		KASSERT((as->as_stackpbase[i] & PAGE_FRAME) == as->as_stackpbase[i]);
	}
#else
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#endif // OPT_A3

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
#if OPT_A3
		rw = 0;
		int frame = (faultaddress - vbase1) / PAGE_SIZE;
		paddr = as->as_pbase1[frame];
#else
		paddr = (faultaddress - vbase1) + as->as_pbase1;
#endif // OPT_A3
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
#if OPT_A3
		rw = TLBLO_DIRTY;
		int frame = (faultaddress - vbase2) / PAGE_SIZE;
		paddr = as->as_pbase2[frame];
#else
		paddr = (faultaddress - vbase2) + as->as_pbase2;
#endif // OPT_A3
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
#if OPT_A3
		rw = TLBLO_DIRTY;
		int frame = (faultaddress - stackbase) / PAGE_SIZE;
		paddr = as->as_stackpbase[frame];
#else
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
#endif // OPT_A3
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

#if OPT_A3
	//Disable rw-checks during address space initialization
	if(as->init) {
		rw = TLBLO_DIRTY;
	}
#endif // OPT_A3

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
#if OPT_A3
		elo = paddr | rw | TLBLO_VALID;
#else
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
#endif // OPT_A3
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

#if OPT_A3
	//Instead of dying, we'll now overwrite the TLB of an entry
	ehi = faultaddress;
	elo = paddr | rw | TLBLO_VALID;
	DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
	tlb_random(ehi, elo);
	splx(spl);
	return 0;
#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif // OPT_A3
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

#if OPT_A3
	as->as_vbase1 = 0;
	as->as_pbase1 = NULL;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = NULL;
	as->as_npages2 = 0;
	as->as_stackpbase = NULL;
	as->init = false;
#else
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;
#endif // OPT_A3

	return as;
}

void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	free_multi_frames(as->as_pbase1, as->as_npages1);
	free_multi_frames(as->as_pbase2, as->as_npages2);
	free_multi_frames(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif // OPT_A3
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
#if OPT_A3
	int i, spl;
	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
#endif // OPT_A3
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

#if OPT_A3
static
void
as_zero_region(paddr_t *paddr, unsigned npages)
{
	for(int i = 0; i < (int) npages; i++) {
		bzero((void *)PADDR_TO_KVADDR(paddr[i]), PAGE_SIZE);
	}
}
#else
static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
#endif // OPT_A3

int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
	KASSERT(as->as_pbase1 == NULL);
	KASSERT(as->as_pbase2 == NULL);
	KASSERT(as->as_stackpbase == NULL);

	as->as_pbase1 = alloc_multi_frames(as->as_npages1);
	if (as->as_pbase1 == NULL) {
		return ENOMEM;
	}

	as->as_pbase2 = alloc_multi_frames(as->as_npages2);
	if (as->as_pbase2 == 0) {
		free_multi_frames(as->as_pbase1, as->as_npages1);
		return ENOMEM;
	}

	as->as_stackpbase = alloc_multi_frames(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		free_multi_frames(as->as_pbase1, as->as_npages1);
		free_multi_frames(as->as_pbase2, as->as_npages2);
		return ENOMEM;
	}

	as->init = true;
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
#endif // OPT_A3
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
#if OPT_A3
	as->init = false;
#else
	(void)as;
#endif // OPT_A3
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

#if OPT_A3
	KASSERT(new->as_pbase1 != NULL);
	KASSERT(new->as_pbase2 != NULL);
	KASSERT(new->as_stackpbase != NULL);

	for(int i = 0; i < (int)old->as_npages1; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase1[i]),
			(const void *)PADDR_TO_KVADDR(old->as_pbase1[i]),
			PAGE_SIZE);
	}

	for(int i = 0; i < (int)old->as_npages2; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase2[i]),
			(const void *)PADDR_TO_KVADDR(old->as_pbase2[i]),
			PAGE_SIZE);
	}

	for(int i = 0; i < DUMBVM_STACKPAGES; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase[i]),
			(const void *)PADDR_TO_KVADDR(old->as_stackpbase[i]),
			PAGE_SIZE);
	}

	as_complete_load(new);
#else
	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
#endif // OPT_A3
	
	*ret = new;
	return 0;
}
