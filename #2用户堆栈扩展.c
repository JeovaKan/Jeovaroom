

/*
 *	存储管理
 */

 	gcc make hello.c

 	$objdump -d hello 


 	call  8048368

 	call指令后面的地址是虚拟地址

 	#define __KERNEL_CS	0x10
	#define __KERNEL_DS	0x18

	#define __USER_CS	0x23
	#define __USER_DS	0x2B
								  index TI RPL	
	__KERNEL_CS			0000 0000 0001 0|0|00

	__KERNEL_DS			0000 0000 0001 1|0|00

	__USER_CS			0000 0000 0010 0|0|11

	__USER_DS			0000 0000 0010 1|0|11

	以32为下标找到gdt表项，并在该值后面添加12个0，以此为地址的指针指向了下一张页面表

	pgd pmd pte 

	按照Intel的规划应该使用3级的表项将虚拟地址转换为物理地址

	实际上linux仅仅使用了pgd和pte来完成。


	每一个物理页面都有一个struct page，系统初始化阶段根据装载的物理内存的大小
	建立一个page结构的数组mem_map，每个物理页面的序号就是在数组中的下标

	mem_map内存仓库中物理页面划分为ZONE_NORMAL和ZONE_DMA两个区域

	每个管理区域都有一组数据结构：

		struct zone 

	由于NUMA结构内存的出现，导致管理趋于不再是最大的管理单位
	对于每个CPU使用的本地内存节点，包含有两个管理区，再包含有页面

	代表着存储节点：struct pglist_data


	以上数据结构都用于物理内存的管理

	struct mm_struct 

		struct vm_area_struct


	物理内存供应，虚拟内存需求，pgd和pte作为桥梁

	缺页异常的缘由

		1. 相应的页面目录项或者页面表项为空，线性地址和物理地址的映射关系尚未建立，或者已经撤销

		2. 相应的物理页面不在内存中

		3. 指令中规定的访问方式与页面的权限不符合

		
	用户堆栈扩展	

	do_page_fault();

		/*
		 * 如果不是向下增长的虚拟内存地址
		 * 说明就不是用户的堆栈空间
		 */
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto bad_area;

		/*
		 * 确实是向下增长，需要判断陷入的空间是否是空洞
		 * 由于intel包含有pusha指令，一次将所有通用寄存器的值
		 * 压入堆栈，所以当前陷入空洞的地址判断从一般的4字节扩展为
		 * 32字节（8个32位寄存器），如果地址仍然超出，那么就是非法申请
		 */
		if (error_code & 4) {
			/*
			 * accessing the stack below %esp is always a bug.
			 * The "+ 32" is there due to some instructions (like
			 * pusha) doing post-decrement on the stack and that
			 * doesn't show up until later..
			 */
			if (address + 32 < regs->esp)
				goto bad_area;
		}
		/*
		 * 好了，我们可以开始扩展用户堆栈了
		 */
		if (expand_stack(vma, address))
			goto bad_area;

			/*
			 * expand_stack 检查资源限制，是否超出task_struct中rlim的资源限制
			 * 是否超出总共可分配的地址空间
			 * 如果一切正常，修改vm_area_struct中的地址。
			 * 所以仅为虚拟地址的扩展，物理页面的映射交给后面的任务完成
			 */	

		good_area:
			info.si_code = SEGV_ACCERR;
			write = 0;
			
			/*
			 * 检查缺页异常传入的错误类型
			 *  
			 * bit1 == 0 说明找不到页面，1说明错误保护
			 * bit2 == 0 说明是读操作中，1说明是写操作
			 * bit3 == 0 说明是内核空间，1说明是用户空间
			 *
			 */
			switch (error_code & 3) {
				default:	/* 3: write, present */
		#ifdef TEST_VERIFY_AREA
					if (regs->cs == KERNEL_CS)
						printk("WP fault at %08lx\n", regs->eip);
		#endif
					/* fall through */
				case 2:		/* write, not present */
					if (!(vma->vm_flags & VM_WRITE))
						goto bad_area;
					write++;
					break;
				case 1:		/* read, present */
					goto bad_area;
				case 0:		/* read, not present */
					if (!(vma->vm_flags & (VM_READ | VM_EXEC)))
						goto bad_area;
			}	 

			/*
			 * 调用虚拟内存管理，正式处理缺页异常
			 */
			switch (handle_mm_fault(mm, vma, address, write)) {
			case 1:
				tsk->min_flt++;
				break;
			case 2:
				tsk->maj_flt++;
				break;
			case 0:
				goto do_sigbus;
			default:
				goto out_of_memory;
			}

				--> handle_mm_fault
				int handle_mm_fault(struct mm_struct *mm, struct vm_area_struct * vma,
					unsigned long address, int write_access)
				{
					int ret = -1;
					pgd_t *pgd;
					pmd_t *pmd;

					/*
					 * 将虚拟地址转换为全局表项
					 */
					pgd = pgd_offset(mm, address);

					/*
					 * 再将全局表项转换为中间表项
					 * 对于Linux来说就是直接转换
					 */
					pmd = pmd_alloc(pgd, address);

					/*
					 * 啊，这个表项是存在的，显然
					 */
					if (pmd) {
						pte_t * pte = pte_alloc(pmd, address);
						if (pte)
							/*
							 * 各个映射的表项都已经建立完毕
							 * 需要处理物理内存页面了
							 */
							ret = handle_pte_fault(mm, vma, address, write_access, pte);
					}
					return ret;
				}

					--> pte_alloc
					extern inline pte_t * pte_alloc(pmd_t * pmd, unsigned long address)
					{
						/*
						 * 将地址转换为pte下标
						 */
						address = (address >> PAGE_SHIFT) & (PTRS_PER_PTE - 1);

						/*
						 * 如果目录项为空。（目录项是pte）
						 * 那么就想办法申请一个新的页面
						 */
						if (pmd_none(*pmd))
							goto getnew;
						if (pmd_bad(*pmd))
							goto fix;
						return (pte_t *)pmd_page(*pmd) + address;
					getnew:
					{	
						/*
						 * 当释放一个页面表的时候，内核先将释放的页面表先
						 * 保存在一个缓冲池中，而不是直接将其物理内存页面
						 * 释放，只有当缓冲池已经满的时候，才真的将页面表
						 * 所占的物理内存页面释放。
						 *
						 * 那么，在分配一个页面表的时候，就先检查一下缓冲池
						 * -->get_pte_fast()，如果缓冲池已经空了（空了？），
						 * 只好通过-->get_pte_slow()，slow~
						 */
						unsigned long page = (unsigned long) get_pte_fast();
						
						if (!page)
							return get_pte_slow(pmd, address);
						set_pmd(pmd, __pmd(_PAGE_TABLE + __pa(page)));
						return (pte_t *)page + address;
					}
					fix:
						__handle_bad_pmd(pmd);
						return NULL;
					}


					--> handle_pte_falult
					static inline int handle_pte_fault(struct mm_struct *mm,
						struct vm_area_struct * vma, unsigned long address,
						int write_access, pte_t * pte)
					{
						pte_t entry;

						/*
						 * We need the page table lock to synchronize with kswapd
						 * and the SMP-safe atomic PTE updates.
						 */
						spin_lock(&mm->page_table_lock);
						entry = *pte;

						/*
						 * 显然对于堆栈扩展的情况，当前的物理内存页面是不存在的
						 * !pte_preasent, pte_none都满足
						 */
						if (!pte_present(entry)) {
							/*
							 * If it truly wasn't present, we know that kswapd
							 * and the PTE updates will not touch it later. So
							 * drop the lock.
							 */
							spin_unlock(&mm->page_table_lock);
							if (pte_none(entry))
								return do_no_page(mm, vma, address, write_access, pte);
							return do_swap_page(mm, vma, address, pte, pte_to_swp_entry(entry), write_access);
						}

						if (write_access) {
							if (!pte_write(entry))
								return do_wp_page(mm, vma, address, pte, entry);

							entry = pte_mkdirty(entry);
						}
						entry = pte_mkyoung(entry);
						establish_pte(vma, address, pte, entry);
						spin_unlock(&mm->page_table_lock);
						return 1;
					}

					--> do_no_page
						static int do_no_page(struct mm_struct * mm, struct vm_area_struct * vma,
							unsigned long address, int write_access, pte_t *page_table)
						{
							struct page * new_page;
							pte_t entry;

							/*
							 * 用户堆栈扩展，不属于特定的文件系统或者页面共享
							 * 则没有特殊的操作函数跳转
							 * --> do_anonymous_page
							 */
							if (!vma->vm_ops || !vma->vm_ops->nopage)
								return do_anonymous_page(mm, vma, page_table, write_access, address);

							/*
							 * The third argument is "no_share", which tells the low-level code
							 * to copy, not share the page even if sharing is possible.  It's
							 * essentially an early COW detection.
							 */
							new_page = vma->vm_ops->nopage(vma, address & PAGE_MASK, (vma->vm_flags & VM_SHARED)?0:write_access);
							if (new_page == NULL)	/* no page was available -- SIGBUS */
								return 0;
							if (new_page == NOPAGE_OOM)
								return -1;
							++mm->rss;
							/*
							 * This silly early PAGE_DIRTY setting removes a race
							 * due to the bad i386 page protection. But it's valid
							 * for other architectures too.
							 *
							 * Note that if write_access is true, we either now have
							 * an exclusive copy of the page, or this is a shared mapping,
							 * so we can make it writable and dirty to avoid having to
							 * handle that later.
							 */
							flush_page_to_ram(new_page);
							flush_icache_page(vma, new_page);
							entry = mk_pte(new_page, vma->vm_page_prot);
							if (write_access) {
								entry = pte_mkwrite(pte_mkdirty(entry));
							} else if (page_count(new_page) > 1 &&
								   !(vma->vm_flags & VM_SHARED))
								entry = pte_wrprotect(entry);
							set_pte(page_table, entry);
							/* no need to invalidate: a not-present page shouldn't be cached */
							update_mmu_cache(vma, address, entry);
							return 2;	/* Major fault */
						}


						--> do_anonymous_page

						static int do_anonymous_page(struct mm_struct * mm, struct vm_area_struct * vma, pte_t *page_table, int write_access, unsigned long addr)
						{
							struct page *page = NULL;
							/*
							 * 首先执行读异常保护
							 * 将RW权限设为0
							 */
							pte_t entry = pte_wrprotect(mk_pte(ZERO_PAGE(addr), vma->vm_page_prot));
							if (write_access) {
								/*
								 * 对于用户堆栈扩展，传入写权限
								 * 只有对于可写的页面，才分配独立的物理内存
								 * 通过alloc_page分配一个物理内存
								 */
								page = alloc_page(GFP_HIGHUSER);
								if (!page)
									return -1;
								clear_user_highpage(page, addr);
								entry = pte_mkwrite(pte_mkdirty(mk_pte(page, vma->vm_page_prot)));
								mm->rss++;
								flush_page_to_ram(page);
							}
							set_pte(page_table, entry);
							/* No need to invalidate - it was non-present before */
							update_mmu_cache(vma, addr, entry);
							return 1;	/* Minor fault */
						}