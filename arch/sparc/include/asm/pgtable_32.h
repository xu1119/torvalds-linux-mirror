/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPARC_PGTABLE_H
#define _SPARC_PGTABLE_H

/*  asm/pgtable.h:  Defines and functions used to work
 *                        with Sparc page tables.
 *
 *  Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 *  Copyright (C) 1998 Jakub Jelinek (jj@sunsite.mff.cuni.cz)
 */

#include <linux/const.h>

#define PMD_SHIFT		18
#define PMD_SIZE        	(1UL << PMD_SHIFT)
#define PMD_MASK        	(~(PMD_SIZE-1))
#define PMD_ALIGN(__addr) 	(((__addr) + ~PMD_MASK) & PMD_MASK)

#define PGDIR_SHIFT     	24
#define PGDIR_SIZE      	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK      	(~(PGDIR_SIZE-1))
#define PGDIR_ALIGN(__addr) 	(((__addr) + ~PGDIR_MASK) & PGDIR_MASK)

#ifndef __ASSEMBLY__
#include <asm-generic/pgtable-nopud.h>

#include <linux/spinlock.h>
#include <linux/mm_types.h>
#include <asm/types.h>
#include <asm/pgtsrmmu.h>
#include <asm/vaddrs.h>
#include <asm/oplib.h>
#include <asm/cpu_type.h>


struct vm_area_struct;
struct page;

void load_mmu(void);
unsigned long calc_highpages(void);
unsigned long __init bootmem_init(unsigned long *pages_avail);

#define pte_ERROR(e)   __builtin_trap()
#define pmd_ERROR(e)   __builtin_trap()
#define pgd_ERROR(e)   __builtin_trap()

#define PTRS_PER_PTE    	64
#define PTRS_PER_PMD    	64
#define PTRS_PER_PGD    	256
#define USER_PTRS_PER_PGD	PAGE_OFFSET / PGDIR_SIZE
#define PTE_SIZE		(PTRS_PER_PTE*4)

#define PAGE_NONE	SRMMU_PAGE_NONE
#define PAGE_SHARED	SRMMU_PAGE_SHARED
#define PAGE_COPY	SRMMU_PAGE_COPY
#define PAGE_READONLY	SRMMU_PAGE_RDONLY
#define PAGE_KERNEL	SRMMU_PAGE_KERNEL

/* Top-level page directory - dummy used by init-mm.
 * srmmu.c will assign the real one (which is dynamically sized) */
#define swapper_pg_dir NULL

void paging_init(void);

extern unsigned long ptr_in_current_pgd;

/* First physical page can be anywhere, the following is needed so that
 * va-->pa and vice versa conversions work properly without performance
 * hit for all __pa()/__va() operations.
 */
extern unsigned long phys_base;
extern unsigned long pfn_base;

/*
 * ZERO_PAGE is a global shared page that is always zero: used
 * for zero-mapped memory areas etc..
 */
extern unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)];

#define ZERO_PAGE(vaddr) (virt_to_page(empty_zero_page))

/*
 * In general all page table modifications should use the V8 atomic
 * swap instruction.  This insures the mmu and the cpu are in sync
 * with respect to ref/mod bits in the page tables.
 */
static inline unsigned long srmmu_swap(unsigned long *addr, unsigned long value)
{
	__asm__ __volatile__("swap [%2], %0" :
			"=&r" (value) : "0" (value), "r" (addr) : "memory");
	return value;
}

/* Certain architectures need to do special things when pte's
 * within a page table are directly modified.  Thus, the following
 * hook is made available.
 */

static inline void set_pte(pte_t *ptep, pte_t pteval)
{
	srmmu_swap((unsigned long *)ptep, pte_val(pteval));
}

static inline int srmmu_device_memory(unsigned long x)
{
	return ((x & 0xF0000000) != 0);
}

static inline unsigned long pmd_pfn(pmd_t pmd)
{
	return (pmd_val(pmd) & SRMMU_PTD_PMASK) >> (PAGE_SHIFT-4);
}

static inline struct page *pmd_page(pmd_t pmd)
{
	if (srmmu_device_memory(pmd_val(pmd)))
		BUG();
	return pfn_to_page(pmd_pfn(pmd));
}

static inline unsigned long __pmd_page(pmd_t pmd)
{
	unsigned long v;

	if (srmmu_device_memory(pmd_val(pmd)))
		BUG();

	v = pmd_val(pmd) & SRMMU_PTD_PMASK;
	return (unsigned long)__nocache_va(v << 4);
}

static inline unsigned long pmd_page_vaddr(pmd_t pmd)
{
	unsigned long v = pmd_val(pmd) & SRMMU_PTD_PMASK;
	return (unsigned long)__nocache_va(v << 4);
}

static inline pmd_t *pud_pgtable(pud_t pud)
{
	if (srmmu_device_memory(pud_val(pud))) {
		return (pmd_t *)~0;
	} else {
		unsigned long v = pud_val(pud) & SRMMU_PTD_PMASK;
		return (pmd_t *)__nocache_va(v << 4);
	}
}

static inline int pte_present(pte_t pte)
{
	return ((pte_val(pte) & SRMMU_ET_MASK) == SRMMU_ET_PTE);
}

static inline int pte_none(pte_t pte)
{
	return !pte_val(pte);
}

static inline void __pte_clear(pte_t *ptep)
{
	set_pte(ptep, __pte(0));
}

static inline void pte_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	__pte_clear(ptep);
}

static inline int pmd_bad(pmd_t pmd)
{
	return (pmd_val(pmd) & SRMMU_ET_MASK) != SRMMU_ET_PTD;
}

static inline int pmd_present(pmd_t pmd)
{
	return ((pmd_val(pmd) & SRMMU_ET_MASK) == SRMMU_ET_PTD);
}

static inline int pmd_none(pmd_t pmd)
{
	return !pmd_val(pmd);
}

static inline void pmd_clear(pmd_t *pmdp)
{
	set_pte((pte_t *)&pmd_val(*pmdp), __pte(0));
}

static inline int pud_none(pud_t pud)
{
	return !(pud_val(pud) & 0xFFFFFFF);
}

static inline int pud_bad(pud_t pud)
{
	return (pud_val(pud) & SRMMU_ET_MASK) != SRMMU_ET_PTD;
}

static inline int pud_present(pud_t pud)
{
	return ((pud_val(pud) & SRMMU_ET_MASK) == SRMMU_ET_PTD);
}

static inline void pud_clear(pud_t *pudp)
{
	set_pte((pte_t *)pudp, __pte(0));
}

/*
 * The following only work if pte_present() is true.
 * Undefined behaviour if not..
 */
static inline int pte_write(pte_t pte)
{
	return pte_val(pte) & SRMMU_WRITE;
}

static inline int pte_dirty(pte_t pte)
{
	return pte_val(pte) & SRMMU_DIRTY;
}

static inline int pte_young(pte_t pte)
{
	return pte_val(pte) & SRMMU_REF;
}

static inline pte_t pte_wrprotect(pte_t pte)
{
	return __pte(pte_val(pte) & ~SRMMU_WRITE);
}

static inline pte_t pte_mkclean(pte_t pte)
{
	return __pte(pte_val(pte) & ~SRMMU_DIRTY);
}

static inline pte_t pte_mkold(pte_t pte)
{
	return __pte(pte_val(pte) & ~SRMMU_REF);
}

static inline pte_t pte_mkwrite_novma(pte_t pte)
{
	return __pte(pte_val(pte) | SRMMU_WRITE);
}

static inline pte_t pte_mkdirty(pte_t pte)
{
	return __pte(pte_val(pte) | SRMMU_DIRTY);
}

static inline pte_t pte_mkyoung(pte_t pte)
{
	return __pte(pte_val(pte) | SRMMU_REF);
}

#define PFN_PTE_SHIFT			(PAGE_SHIFT - 4)

static inline pte_t pfn_pte(unsigned long pfn, pgprot_t pgprot)
{
	return __pte((pfn << PFN_PTE_SHIFT) | pgprot_val(pgprot));
}

static inline unsigned long pte_pfn(pte_t pte)
{
	if (srmmu_device_memory(pte_val(pte))) {
		/* Just return something that will cause
		 * pfn_valid() to return false.  This makes
		 * copy_one_pte() to just directly copy to
		 * PTE over.
		 */
		return ~0UL;
	}
	return (pte_val(pte) & SRMMU_PTE_PMASK) >> PFN_PTE_SHIFT;
}

#define pte_page(pte)	pfn_to_page(pte_pfn(pte))

static inline pte_t mk_pte_phys(unsigned long page, pgprot_t pgprot)
{
	return __pte(((page) >> 4) | pgprot_val(pgprot));
}

static inline pte_t mk_pte_io(unsigned long page, pgprot_t pgprot, int space)
{
	return __pte(((page) >> 4) | (space << 28) | pgprot_val(pgprot));
}

#define pgprot_noncached pgprot_noncached
static inline pgprot_t pgprot_noncached(pgprot_t prot)
{
	pgprot_val(prot) &= ~pgprot_val(__pgprot(SRMMU_CACHE));
	return prot;
}

static pte_t pte_modify(pte_t pte, pgprot_t newprot) __attribute_const__;
static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
	return __pte((pte_val(pte) & SRMMU_CHG_MASK) |
		pgprot_val(newprot));
}

/* only used by the huge vmap code, should never be called */
#define pud_page(pud)			NULL

struct seq_file;
void mmu_info(struct seq_file *m);

/* Fault handler stuff... */
#define FAULT_CODE_PROT     0x1
#define FAULT_CODE_WRITE    0x2
#define FAULT_CODE_USER     0x4

#define update_mmu_cache(vma, address, ptep) do { } while (0)
#define update_mmu_cache_range(vmf, vma, address, ptep, nr) do { } while (0)

void srmmu_mapiorange(unsigned int bus, unsigned long xpa,
                      unsigned long xva, unsigned int len);
void srmmu_unmapiorange(unsigned long virt_addr, unsigned int len);

/*
 * Encode/decode swap entries and swap PTEs. Swap PTEs are all PTEs that
 * are !pte_none() && !pte_present().
 *
 * Format of swap PTEs:
 *
 *   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
 *   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 *   <-------------- offset ---------------> < type -> E 0 0 0 0 0 0
 */
static inline unsigned long __swp_type(swp_entry_t entry)
{
	return (entry.val >> SRMMU_SWP_TYPE_SHIFT) & SRMMU_SWP_TYPE_MASK;
}

static inline unsigned long __swp_offset(swp_entry_t entry)
{
	return (entry.val >> SRMMU_SWP_OFF_SHIFT) & SRMMU_SWP_OFF_MASK;
}

static inline swp_entry_t __swp_entry(unsigned long type, unsigned long offset)
{
	return (swp_entry_t) {
		(type & SRMMU_SWP_TYPE_MASK) << SRMMU_SWP_TYPE_SHIFT
		| (offset & SRMMU_SWP_OFF_MASK) << SRMMU_SWP_OFF_SHIFT };
}

#define __pte_to_swp_entry(pte)		((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)		((pte_t) { (x).val })

static inline bool pte_swp_exclusive(pte_t pte)
{
	return pte_val(pte) & SRMMU_SWP_EXCLUSIVE;
}

static inline pte_t pte_swp_mkexclusive(pte_t pte)
{
	return __pte(pte_val(pte) | SRMMU_SWP_EXCLUSIVE);
}

static inline pte_t pte_swp_clear_exclusive(pte_t pte)
{
	return __pte(pte_val(pte) & ~SRMMU_SWP_EXCLUSIVE);
}

static inline unsigned long
__get_phys (unsigned long addr)
{
	switch (sparc_cpu_model){
	case sun4m:
	case sun4d:
		return ((srmmu_get_pte (addr) & 0xffffff00) << 4);
	default:
		return 0;
	}
}

static inline int
__get_iospace (unsigned long addr)
{
	switch (sparc_cpu_model){
	case sun4m:
	case sun4d:
		return (srmmu_get_pte (addr) >> 28);
	default:
		return -1;
	}
}

/*
 * For sparc32&64, the pfn in io_remap_pfn_range() carries <iospace> in
 * its high 4 bits.  These macros/functions put it there or get it from there.
 */
#define MK_IOSPACE_PFN(space, pfn)	(pfn | (space << (BITS_PER_LONG - 4)))
#define GET_IOSPACE(pfn)		(pfn >> (BITS_PER_LONG - 4))
#define GET_PFN(pfn)			(pfn & 0x0fffffffUL)

int remap_pfn_range(struct vm_area_struct *, unsigned long, unsigned long,
		    unsigned long, pgprot_t);

static inline int io_remap_pfn_range(struct vm_area_struct *vma,
				     unsigned long from, unsigned long pfn,
				     unsigned long size, pgprot_t prot)
{
	unsigned long long offset, space, phys_base;

	offset = ((unsigned long long) GET_PFN(pfn)) << PAGE_SHIFT;
	space = GET_IOSPACE(pfn);
	phys_base = offset | (space << 32ULL);

	return remap_pfn_range(vma, from, phys_base >> PAGE_SHIFT, size, prot);
}
#define io_remap_pfn_range io_remap_pfn_range

#define __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
#define ptep_set_access_flags(__vma, __address, __ptep, __entry, __dirty) \
({									  \
	int __changed = !pte_same(*(__ptep), __entry);			  \
	if (__changed) {						  \
		set_pte(__ptep, __entry);				  \
		flush_tlb_page(__vma, __address);			  \
	}								  \
	__changed;							  \
})

#endif /* !(__ASSEMBLY__) */

#define VMALLOC_START           _AC(0xfe600000,UL)
#define VMALLOC_END             _AC(0xffc00000,UL)
#define MODULES_VADDR           VMALLOC_START
#define MODULES_END             VMALLOC_END

/* We provide our own get_unmapped_area to cope with VA holes for userland */
#define HAVE_ARCH_UNMAPPED_AREA

#define pmd_pgtable(pmd)	((pgtable_t)__pmd_page(pmd))

#endif /* !(_SPARC_PGTABLE_H) */
