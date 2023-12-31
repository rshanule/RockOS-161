
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <vnode.h>
#include <vfs.h>
#include <stat.h>
#include <uio.h>
#include <kern/fcntl.h>
#include <rand.h>

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

static unsigned long pagesAlloced = 0;
static unsigned int used =  0;
static int robinPointer;

bool swapping_enabled;
int disksize = 0;
struct vnode* swapDisk;
struct lock* swapLock;
struct bitmap* swapMap;

void vm_bootstrap(){
	robinPointer = paddr_to_index(getFirstPaddr());
	swapDisk = kmalloc(sizeof(struct vnode));
	if(swapDisk == NULL){
		panic("swapDisk null in vm_bootstrap");
	}
	char foo [] = "lhd0raw:";
	int res = vfs_open(foo, O_RDWR, 0, &swapDisk);
	if(res > 0){
		swapping_enabled = false;
		disksize = 0;
	 	kprintf("Swapdisk not mounted\n");
	 	kfree(swapDisk);
	 	return;
	}
	swapping_enabled = true;
	struct stat* statBox = kmalloc(sizeof(struct stat));
	if(statBox == NULL){
		panic("statBox null in vm_bootstrap");
	}
	VOP_STAT(swapDisk, statBox);
	disksize = statBox->st_size;
	kfree(statBox);
	
	//Initialize the swapdisk bitmap
	swapMap = bitmap_create((unsigned)(disksize/PAGE_SIZE));
	
	kprintf("Disk size (bytes): %d\n",  disksize);
	kprintf("Bitmap size: %d\n",  disksize/PAGE_SIZE);

	swapLock = lock_create("swap_lock");
	copyBuffer_init();
	(void) statBox;
	(void) swapDisk;

	return;
}


/*
 * Level of indirection not necessary, but allows us to swap between ASST2 and ASST3 
 * configurations without breaking
 */
void coremap_bootstrap(void){
	coremap_init();
}

static
paddr_t
getppages(unsigned long npages, bool user, struct pte* owner){
	
	spinlock_acquire(&stealmem_lock);
	paddr_t ramsize = ram_getsize();
	// PAGE_SIZE defined in arch/mips/include/vm.h
	int numberOfEntries = (int)ramsize/PAGE_SIZE;
	int startOfCurBlock = 0;
	unsigned long contiguousFound = 0;
	for(int i = 0; i < numberOfEntries; i++){
		if(contiguousFound == npages){
			for(int j = startOfCurBlock; (unsigned long)j < startOfCurBlock + npages; j++){
				get_corePage(j)->allocated = 1;
				get_corePage(j)->firstpage = startOfCurBlock;
				get_corePage(j)->npages = npages;
				get_corePage(j)->user = user;
				get_corePage(j)->owner_pte = owner;
				get_corePage(j)->clockbit = true;
			}
			used += PAGE_SIZE * npages;
			pagesAlloced += npages;
			paddr_t addr = index_to_pblock(startOfCurBlock);
			spinlock_release(&stealmem_lock);
			
			return addr; 
			
		}
		if(get_corePage(i)->allocated == 1){
			contiguousFound = 0;
			startOfCurBlock = i + 1;
			continue;
		}
		contiguousFound++;

	}

	spinlock_release(&stealmem_lock);

	//Contiguous block not found.
	if(!swapping_enabled){
	 	return (paddr_t)0;
	}
	 
	//PAGE OUT
	bool gotLockEarlier = false;

	if(swapping_enabled && !lock_do_i_hold(swapLock)){
		gotLockEarlier = true;
		lock_acquire(swapLock);
	}

	//Clock eviction
	bool upage_found = false;
	int victimIndex = -1;
	while(!upage_found){
	 	if(get_corePage(robinPointer)->user){
	 		if(get_corePage(robinPointer)->clockbit){
	 			get_corePage(robinPointer)->clockbit = false;
	 		}
	 		else{
	 			get_corePage(robinPointer)->clockbit = true;
		 		upage_found = true;
		 		victimIndex = robinPointer;
	 		}
	 	}
	 	robinPointer++;
	 	if(robinPointer >= numberOfEntries){
	 		robinPointer = paddr_to_index(getFirstPaddr());
	 	}
	}

	//Lock down PTE
	struct corePage* victimPage = get_corePage(victimIndex);
	struct lock* ptelock = victimPage->owner_pte->pte_lock;
	lock_acquire(ptelock);
	//Clear entry in TLB if it exists
	
	if(victimPage->owner_thread->t_state == S_RUN){
		if(victimPage->owner_thread->t_cpu == curcpu){
			int spl = splhigh();
	
			int tlbprobe = tlb_probe(victimPage->owner_pte->vpn, 0);
			if(tlbprobe >= 0){
			tlb_write(TLBHI_INVALID(tlbprobe), TLBLO_INVALID(), tlbprobe);
			}
			splx(spl);
	
		}
		else{
			struct tlbshootdown* shooter = kmalloc(sizeof(*shooter));
			shooter->ts_placeholder = (int)victimPage->owner_pte->vpn;
			ipi_tlbshootdown(victimPage->owner_thread->t_cpu, shooter);
			kfree(shooter);
			
		}

	}

	//Find and mark a free index in the swapDisk
	unsigned destIndex;
	int err = bitmap_alloc(swapMap, &destIndex);

	//ERROR if no space in swapmap
	if(err){
	 	panic("Swapdisk full in getppages");
	 	return 0;
	}

	//Write page to swap disk
	blockwrite(PADDR_TO_KVADDR(index_to_pblock((unsigned int)victimIndex)), destIndex);
	
	//Inform the owner
	if(victimPage->owner_pte == NULL){
	 	kprintf("%d\n", victimPage->user);
	 	panic("Fuck\n");
	}
	victimPage->owner_pte->inmem = false;
	victimPage->owner_pte->swapIndex = destIndex;
	 
	//Finalize new allocation
	//We can assume allocated is already 1
	//We can assume firstpage is already victimIndex
	//We can assume npages is already 1
	//Update the owner
	victimPage->user = user;
	victimPage->owner_pte = owner;
	lock_release(ptelock);
	if(swapping_enabled && gotLockEarlier){
		lock_release(swapLock);
	}

	paddr_t addr = index_to_pblock(victimIndex);

	return addr;

}


/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(unsigned npages){
	
	paddr_t startOfNewBlock = getppages(npages, false, NULL);
	
	if (startOfNewBlock==0) {
			return 0;
		}
	return PADDR_TO_KVADDR(startOfNewBlock);

}

vaddr_t alloc_upages(unsigned npages, struct pte* owner){

	paddr_t startOfNewBlock = getppages(npages, true, owner);
	if (startOfNewBlock==0) {
			return 0;
	}
	get_corePage(paddr_to_index(startOfNewBlock))->owner_thread = curthread;
	bzero((void*)PADDR_TO_KVADDR(startOfNewBlock), npages * PAGE_SIZE);
	return PADDR_TO_KVADDR(startOfNewBlock);

}

vaddr_t alloc_kpages_nozero(unsigned npages){
	
	paddr_t startOfNewBlock = getppages(npages, false, NULL);
	
	if (startOfNewBlock==0) {
			return 0;
	}

	return PADDR_TO_KVADDR(startOfNewBlock);
}

void free_kpages(vaddr_t addr){

	spinlock_acquire(&stealmem_lock);
	vaddr_t base = PADDR_TO_KVADDR(index_to_pblock(0));
	int page = (addr - base) / PAGE_SIZE;
	int basePage =  get_corePage(page)->firstpage;
	int npages = get_corePage(basePage)->npages;

	for(int i = basePage; i < basePage + npages; ++i){
		get_corePage(i)->allocated = 0;
		get_corePage(i)->firstpage = -1;
		get_corePage(i)->npages = 0;
		get_corePage(i)->user = false;
		get_corePage(i)->owner_pte = NULL;
		get_corePage(i)->owner_thread = NULL;
		
	}
	
	used -= (npages * PAGE_SIZE);
	spinlock_release(&stealmem_lock);

	return;
}

/*
 * Return amount of memory (in bytes) used by allocated coremap pages.  If
 * there are ongoing allocations, this value could change after it is returned
 * to the caller. But it should have been correct at some point in time.
 */
unsigned int coremap_used_bytes(void){
	return used;
}

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown * tlbs){
	
	int spl = splhigh();
	
	int tlbprobe = tlb_probe((vaddr_t)tlbs->ts_placeholder, 0);
	if(tlbprobe >= 0){
		tlb_write(TLBHI_INVALID(tlbprobe), TLBLO_INVALID(), tlbprobe);
	}
	
	splx(spl);

	return;
}

/*
 * Copies a page from disk to memory. (source, destination)
 */
void blockread(int swapIndex, vaddr_t vaddr){
	(void)swapIndex;
	(void)vaddr;

	vaddr &= PAGE_FRAME;	//Aligns paddr to the start of a page

	struct uio thing;
	struct iovec iov;
	iov.iov_kbase = (void*)vaddr;
	iov.iov_len = PAGE_SIZE;		
	thing.uio_iov = &iov;
	thing.uio_iovcnt = 1;
	thing.uio_resid = PAGE_SIZE; 
	thing.uio_offset = swapIndex * PAGE_SIZE;	//Offset into swapDisk to which to write
	thing.uio_segflg = UIO_SYSSPACE;
	thing.uio_rw = UIO_READ;
	thing.uio_space = NULL;

	VOP_READ(swapDisk, &thing);

	return;
}

/*
 * Copies a page from memory to disk. (source, destination)
 * paddr is an address in the block of memory to be copied
 * swapIndex is the index in the swapDisk to which the page is to be copied.
 */
void blockwrite(vaddr_t vaddr, int swapIndex){
	(void)swapIndex;
	(void)vaddr;

	vaddr &= PAGE_FRAME;	//Aligns paddr to the start of a page

	struct uio thing;
	struct iovec iov;
	iov.iov_kbase = (void*)vaddr;	
	iov.iov_len = PAGE_SIZE;		
	thing.uio_iov = &iov;
	thing.uio_iovcnt = 1;
	thing.uio_resid = PAGE_SIZE; 
	thing.uio_offset = swapIndex * PAGE_SIZE;	//Offset into swapDisk to which to write
	thing.uio_segflg = UIO_SYSSPACE;
	thing.uio_rw = UIO_WRITE;
	thing.uio_space = NULL;

	VOP_WRITE(swapDisk, &thing);

	return;
}

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress){

	int spl;
	uint32_t ehi, elo;


	faultaddress &= PAGE_FRAME;
	ehi = faultaddress;
	//Check if address is in valid region
	LinkedList* curreg = curthread->t_proc->p_addrspace->regions->next;
	
	if(curreg == NULL){
		return EFAULT;
	}

	while(1){
		vaddr_t regstart = ((struct region*)(curreg->data))->start;
		vaddr_t regend = ((struct region*)(curreg->data))->end;
		if(faultaddress >= regstart && faultaddress < regend){
			//Found valid region. Must search page table for vpn.
			LinkedList* curpte = curthread->t_proc->p_addrspace->pageTable;
			while(1){
				struct lock* ptelock = ((struct pte*)curpte->data)->pte_lock;
				if((faultaddress) == ((struct pte*)curpte->data)->vpn){
					
					if(swapping_enabled) lock_acquire(ptelock);
					//Page is in table. Now check if it's in memory.
					if(((struct pte*)curpte->data)->inmem){
						//Verified page in memory. Now we need to load the TLB
						
						elo = (uint32_t)(((struct pte*)curpte->data)->ppn);
						get_corePage(paddr_to_index((paddr_t)elo))->clockbit = true;
						
						spl = splhigh();
						
						if(tlb_probe(ehi, elo) >= 0){
							tlb_write(ehi, elo | TLBLO_DIRTY | TLBLO_VALID, tlb_probe(ehi, elo | TLBLO_DIRTY | TLBLO_VALID));
							
							splx(spl);
							if(swapping_enabled && ptelock != NULL) lock_release(ptelock);							
							return 0;
						}
						tlb_random(ehi, elo | TLBLO_DIRTY | TLBLO_VALID);

						splx(spl);

						if(swapping_enabled && ptelock != NULL) lock_release(ptelock);
						return 0;
					}
					//Page not in memory (Page fault). Must PAGE IN:
					if(!swapping_enabled){
						panic("This should never be true here (vm_vault)\n");
					}

 					//Lock down PTE

					//Allocate new page to swap in to
					if(swapping_enabled && ptelock != NULL){
						lock_acquire(swapLock);
					} 
						
					
					vaddr_t allocAddr = alloc_upages(1, (struct pte*)curpte->data);
					
					//Update ppn in pte to the new physical allocation address
					((struct pte*)curpte->data)->ppn = allocAddr - 0x80000000;
					//Copy data from swapDisk to the new address
					blockread(((struct pte*)curpte->data)->swapIndex, PADDR_TO_KVADDR(((struct pte*)curpte->data)->ppn));
					//Clear that index in the swapMap
					bitmap_unmark(swapMap, ((struct pte*)curpte->data)->swapIndex);
					//Inform the pte that the page is back in memory
					((struct pte*)curpte->data)->inmem = true;
					((struct pte*)curpte->data)->swapIndex = -1;
					
					if(swapping_enabled && ptelock != NULL){
						lock_release(ptelock);
						lock_release(swapLock);
					} 
					return 0;
				

				}
				if(LLnext(curpte) == NULL){
					break;
				}
				curpte = LLnext(curpte);
			}
			//Failed to find page in table. Must allocate new one.

			struct pte* newPage = kmalloc(sizeof(*newPage));
			vaddr_t allocAddr = alloc_upages(1, newPage);

			newPage->vpn = faultaddress;
			newPage->ppn = (allocAddr - 0x80000000);
			newPage->inmem = true;
			newPage->swapIndex = -1;
			newPage->pte_lock = lock_create("insert spongebob meme here");
			lock_acquire(newPage->pte_lock);
			LLaddWithDatum((char*)"weast", newPage, curpte);
			
			/*
			 * Frob TLB 
			 * This code is not actually necessary as we can let it fault again 
			 * and use the code above instead.
			 */
			elo = newPage->ppn;
			get_corePage(paddr_to_index((paddr_t)elo))->clockbit = true;
			
			spl = splhigh();
			
			if(tlb_probe(ehi, elo) >= 0){
				tlb_write(ehi, elo | TLBLO_DIRTY | TLBLO_VALID, tlb_probe(ehi, elo | TLBLO_DIRTY | TLBLO_VALID));
				splx(spl);
				return 0;
			}
			tlb_random(ehi, elo | TLBLO_DIRTY | TLBLO_VALID);
			
			splx(spl);
			lock_release(newPage->pte_lock);
			return 0;

		}

		if(LLnext(curreg) == NULL){
			break;
		}

		curreg = LLnext(curreg);
	}
	//Seg fault
	(void)faulttype;
	(void)faultaddress;
	return EFAULT;
}
