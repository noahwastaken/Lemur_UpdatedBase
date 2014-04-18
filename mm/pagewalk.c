#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>

static int walk_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pte_t *pte;
	int err = 0;

	pte = pte_offset_map(pmd, addr);
	for (;;) {
		err = walk->pte_entry(pte, addr, addr + PAGE_SIZE, walk);
		if (err)
		       break;
		addr += PAGE_SIZE;
		if (addr == end)
			break;
		pte++;
	}

	pte_unmap(pte);
	return err;
}

static int walk_pmd_range(pud_t *pud, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pmd_t *pmd;
	unsigned long next;
	int err = 0;

	pmd = pmd_offset(pud, addr);
	do {
again:
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd)) {
			if (walk->pte_hole)
				err = walk->pte_hole(addr, next, walk);
			if (err)
				break;
			continue;
		}
		if (walk->pmd_entry)
			err = walk->pmd_entry(pmd, addr, next, walk);
		if (err)
			break;

		if (!walk->pte_entry)
			continue;

		split_huge_page_pmd(walk->mm, pmd);
		if (pmd_none_or_trans_huge_or_clear_bad(pmd))
			goto again;
		err = walk_pte_range(pmd, addr, next, walk);
		if (err)
			break;
	} while (pmd++, addr = next, addr != end);

	return err;
}

static int walk_pud_range(pgd_t *pgd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pud_t *pud;
	unsigned long next;
	int err = 0;

	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud)) {
			if (walk->pte_hole)
				err = walk->pte_hole(addr, next, walk);
			if (err)
				break;
			continue;
		}
		if (walk->pud_entry)
			err = walk->pud_entry(pud, addr, next, walk);
		if (!err && (walk->pmd_entry || walk->pte_entry))
			err = walk_pmd_range(pud, addr, next, walk);
		if (err)
			break;
	} while (pud++, addr = next, addr != end);

	return err;
}

#ifdef CONFIG_HUGETLB_PAGE
static unsigned long hugetlb_entry_end(struct hstate *h, unsigned long addr,
				       unsigned long end)
{
	unsigned long boundary = (addr & huge_page_mask(h)) + huge_page_size(h);
	return boundary < end ? boundary : end;
}

static int walk_hugetlb_range(struct vm_area_struct *vma,
			      unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
	struct hstate *h = hstate_vma(vma);
	unsigned long next;
	unsigned long hmask = huge_page_mask(h);
	pte_t *pte;
	int err = 0;

	do {
		next = hugetlb_entry_end(h, addr, end);
		pte = huge_pte_offset(walk->mm, addr & hmask);
		if (pte && walk->hugetlb_entry)
			err = walk->hugetlb_entry(pte, hmask, addr, next, walk);
		if (err)
			return err;
	} while (addr = next, addr != end);

	return 0;
}

#else 

static int walk_hugetlb_range(struct vm_area_struct *vma,
			      unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
	return 0;
}

#endif 



int walk_page_range(unsigned long addr, unsigned long end,
		    struct mm_walk *walk)
{
	pgd_t *pgd;
	unsigned long next;
	int err = 0;

	if (addr >= end)
		return err;

	if (!walk->mm)
		return -EINVAL;

	VM_BUG_ON(!rwsem_is_locked(&walk->mm->mmap_sem));

	pgd = pgd_offset(walk->mm, addr);
	do {
		struct vm_area_struct *vma = NULL;

		next = pgd_addr_end(addr, end);

		/*
		 * This function was not intended to be vma based.
		 * But there are vma special cases to be handled:
		 * - hugetlb vma's
		 * - VM_PFNMAP vma's
 		 */
		vma = find_vma(walk->mm, addr);
		if (vma) {
			/*
			 * There are no page structures backing a VM_PFNMAP
			 * range, so do not allow split_huge_page_pmd().
			 */
			if ((vma->vm_start <= addr) &&
			    (vma->vm_flags & VM_PFNMAP)) {
				next = vma->vm_end;
				pgd = pgd_offset(walk->mm, next);
				continue;
			}
			/*
			 * Handle hugetlb vma individually because pagetable
			 * walk for the hugetlb page is dependent on the
			 * architecture and we can't handled it in the same
			 * manner as non-huge pages.
 			 */
			if (walk->hugetlb_entry && (vma->vm_start <= addr) &&
			    is_vm_hugetlb_page(vma)) {
				if (vma->vm_end < next)
					next = vma->vm_end;
				/*
				 * Hugepage is very tightly coupled with vma,
				 * so walk through hugetlb entries within a
				 * given vma.
				 */
				err = walk_hugetlb_range(vma, addr, next, walk);
				if (err)
					break;
				pgd = pgd_offset(walk->mm, next);
				continue;
			}
		}

		if (pgd_none_or_clear_bad(pgd)) {
			if (walk->pte_hole)
				err = walk->pte_hole(addr, next, walk);
			if (err)
				break;
			pgd++;
			continue;
		}
		if (walk->pgd_entry)
			err = walk->pgd_entry(pgd, addr, next, walk);
		if (!err &&
		    (walk->pud_entry || walk->pmd_entry || walk->pte_entry))
			err = walk_pud_range(pgd, addr, next, walk);
		if (err)
			break;
		pgd++;
	} while (addr = next, addr != end);

	return err;
}
