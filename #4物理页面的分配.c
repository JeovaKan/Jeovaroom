
/*
 * 物理页面的分配
 */

 UMA与NUMA结构内存的分配函数，根据编译选项CONFIG_DISCONTIGMEM选择

 mm/numa.c

 	--> alloc_page()
 								  /*  哪一种分配策略         分配的阶数   */
	 	struct page * alloc_pages(int gfp_mask,     unsigned long order)
		{
			struct page *ret = 0;
			pg_data_t *start, *temp;

		/*
		 * 广义上的NUMA，有空洞的存储空间也是一种非连续
		 */	
		#ifndef CONFIG_NUMA
			unsigned long flags;
			static pg_data_t *next = 0;
		#endif

			if (order >= MAX_ORDER)
				return NULL;
		/*
		 * 找到cpu所在节点的pg_data_t数据结构队列
		 */		
		#ifdef CONFIG_NUMA
			temp = NODE_DATA(numa_node_id());
		#else
			spin_lock_irqsave(&node_lock, flags);
			if (!next) next = pgdat_list;
			temp = next;
			next = next->node_next;
			spin_unlock_irqrestore(&node_lock, flags);
		#endif
			start = temp;
			/*
			 * 两个循环，分两部分在队列中寻找合适的页面
			 * 首先从节点到结尾，再从节点到开始
			 */
			while (temp) {
				/* 
				 * 对于循环中的每一个节点，尝试分配所需的页面
				 */
				if ((ret = alloc_pages_pgdat(temp, gfp_mask, order)))
					return(ret);
				temp = temp->node_next;
			}
			temp = pgdat_list;
			while (temp != start) {
				if ((ret = alloc_pages_pgdat(temp, gfp_mask, order)))
					return(ret);
				temp = temp->node_next;
			}
			return(0);
		}

		--> alloc_pages_pgdat()
			/*
			 * gfp_mask作为下标，即为分配策略的选择
			 */
			static struct page * alloc_pages_pgdat(pg_data_t *pgdat, int gfp_mask,
				unsigned long order)
			{
				return __alloc_pages(pgdat->node_zonelists + gfp_mask, order);
			}

		UMA的区别在于，传入参数仅有一个节点contig_page_data

		mm/mm.h

		--> alloc_pages()

			static inline struct page * alloc_pages(int gfp_mask, unsigned long order)
			{
				if (order >= MAX_ORDER)
					return NULL;
				return __alloc_pages(contig_page_data.node_zonelists+(gfp_mask), order);
			}	

			以上最终调用的分配函数：

			--> __alloc_pages
										    /*         某个具体的分配策略|      阶数  */
				struct page * __alloc_pages(zonelist_t *zonelist, unsigned long order)
				{
					zone_t **zone;
					/*
					 * direct_reclaim如果设置为1，可以从不活跃干净队列中回收
					 */
					int direct_reclaim = 0;

					/* 当前的具体分配策略 */
					unsigned int gfp_mask = zonelist->gfp_mask;
					struct page * page;

					/*
					 * 全局变量，内存页面管理的压力，分配时++，释放时--
					 */
					memory_pressure++;

					/*
					 * 对于单个页面，则从不活跃干净队列中回收
					 * 另外两个条件还不知道0-0
					 */
					if (order == 0 && (gfp_mask & __GFP_WAIT) &&
							!(current->flags & PF_MEMALLOC))
						direct_reclaim = 1;

					/*
					 * 当页面短缺的时候，使用kswapd和bdflush线程页面定期回收
					 * 目前还不知道具体实现0-0
					 */
					if (inactive_shortage() > inactive_target / 2 && free_shortage())
						wakeup_kswapd(0);
					else if (free_shortage() && nr_inactive_dirty_pages > free_shortage()
							&& nr_inactive_dirty_pages >= freepages.high)
						wakeup_bdflush(0);

				try_again:

					zone = zonelist->zones;
					for (;;) {
						/*
						 * 如果当前的区所有空闲队列都失败了
						 * 就进入下一个管理区
						 */
						zone_t *z = *(zone++);
						if (!z)
							break;
						if (!z->size)
							BUG();

						if (z->free_pages >= z->pages_low) {
							/* 对于满足条件的区，分配order需求的页面 */
							page = rmqueue(z, order);
							if (page)
								return page;

						/* 如果当前区的余量已经低于水位了，就唤醒kreclaimd_wait */
						} else if (z->free_pages < z->pages_min &&
									waitqueue_active(&kreclaimd_wait)) {
								wake_up_interruptible(&kreclaimd_wait);
						}
					}
						-->rmqueue()
						{/* 仅作为摘要使用，非此处的调用 */
							static struct page * rmqueue(zone_t *zone, unsigned long order)
							{
								free_area_t * area = zone->free_area + order;
								unsigned long curr_order = order;
								struct list_head *head, *curr;
								unsigned long flags;
								struct page *page;

								spin_lock_irqsave(&zone->lock, flags);
								do {
									head = &area->free_list;
									curr = memlist_next(head);

									if (curr != head) {
										unsigned int index;

										page = memlist_entry(curr, struct page, list);
										if (BAD_RANGE(zone,page))
											BUG();
										memlist_del(curr);
										index = (page - mem_map) - zone->offset;
										MARK_USED(index, curr_order, area);
										zone->free_pages -= 1 << order;

										page = expand(zone, page, index, order, curr_order, area);
										spin_unlock_irqrestore(&zone->lock, flags);

										/*
										 * 申请成功，当前使用者+1
										 */
										set_page_count(page, 1);
										if (BAD_RANGE(zone,page))
											BUG();
										DEBUG_ADD_PAGE
										return page;	
									}
									curr_order++;
									area++;
								} while (curr_order < MAX_ORDER);
								spin_unlock_irqrestore(&zone->lock, flags);

								return NULL;
							}

						}

							--> expand()/* 解释作用，此处的申明 */
							/*
							 * 如果分配是由较大的块划分来的，那么剩下的部分
							 * 就要分解成较小的块并划入对应的队列中
							 * 入参中，low代表所需要的物理块大小的order
							 * high代表当前空闲队列的order
							 */
							static inline struct page * expand (zone_t *zone, struct page *page,unsigned long index, int low, int high, free_area_t * area)
							{
								unsigned long size = 1 << high;

								while (high > low) {
									if (BAD_RANGE(zone,page))
										BUG();
									area--;
									high--;
									size >>= 1;
									memlist_add_head(&(page)->list, &(area)->free_list);
									MARK_USED(index, high, area);
									index += size;
									page += size;
								}
								if (BAD_RANGE(zone,page))
									BUG();
								return page;
							}

					/*
					 * 分配策略中所有页面管理区都分配失败了
					 * 只好再次尝试，降低水位的要求，同时把
					 * 不活跃干净页面考虑进去
					 */
					page = __alloc_pages_limit(zonelist, order, PAGES_HIGH, direct_reclaim);
					if (page)
						return page;

					/*
					 * 先用PAGES_HIGH入参，再用PAGES_LOW入参
					 * 加大力度
					 */
					page = __alloc_pages_limit(zonelist, order, PAGES_LOW, direct_reclaim);
					if (page)
						return page;

						-->__alloc_pages_limit

						static struct page * __alloc_pages_limit(zonelist_t *zonelist,
								unsigned long order, int limit, int direct_reclaim)
						{
							zone_t **zone = zonelist->zones;

							for (;;) {
								zone_t *z = *(zone++);
								unsigned long water_mark;

								if (!z)
									break;
								if (!z->size)
									BUG();

								/*
								 * 根据入参修改水位的限制
								 */
								switch (limit) {
									default:
									case PAGES_MIN:
										water_mark = z->pages_min;
										break;
									case PAGES_LOW:
										water_mark = z->pages_low;
										break;
									case PAGES_HIGH:
										water_mark = z->pages_high;
								}

								if (z->free_pages + z->inactive_clean_pages > water_mark) {
									struct page *page = NULL;
									/* If possible, reclaim a page directly. */
									if (direct_reclaim && z->free_pages < z->pages_min + 8)	
										/* 此处用于页面定期回收 */
										page = reclaim_page(z);
									/* If that fails, fall back to rmqueue. */
									if (!page)
										page = rmqueue(z, order);
									if (page)
										return page;
								}
							}

							/* Found nothing. */
							return NULL;
						}

					/*
					 * 修改水位限制以后依然无法获取页面
					 * 0_0怎么解决
					 */
					/* 唤醒kswapd进程 */
					wakeup_kswapd(0);

					/* 
					 * 如果分配策略是必须分配得到，宁可等待也要完成
					 * 就让系统调度一次，为其它进程让路。
					 * 1.kswapd可能立即被调度一次（0_0???）
					 * 2.其它进程可能会释放一些页面
					 */
					if (gfp_mask & __GFP_WAIT) {
						__set_current_state(TASK_RUNNING);
						current->policy |= SCHED_YIELD;
						schedule();
					}

					/*
					 * 当该进程再次被调度的时候
					 * 就用PAGES_MIN的水位再次尝试一次
					 */
					page = __alloc_pages_limit(zonelist, order, PAGES_MIN, direct_reclaim);
					if (page)
						return page;

					/*
					 * 握草，到这里说明上面的方案还是失败了
					 * 1. 申请的页面order太大了，list里面有单个或者不足够
					 *    数量的页面，此时需要交换一些脏的页面出去，以补足
					 * 2. 系统内存真的太少了，必须等待页面回收
					 */
					if (!(current->flags & PF_MEMALLOC)) {
						/*
						 * Are we dealing with a higher order allocation?
						 * （目前在处理一个大order的申请）
						 * Move pages from the inactive_clean to the free list
						 * in the hope of creating a large, physically contiguous
						 * piece of free memory.
						 */
						if (order > 0 && (gfp_mask & __GFP_WAIT)) {
							zone = zonelist->zones;
							
							/*
							 * 页面换出，以洗净脏的页面
							 * 设置PF_MEMALLOC，防止递归调用（0_0先跳过这条）
							 */
							current->flags |= PF_MEMALLOC;
							page_launder(gfp_mask, 1);
							current->flags &= ~PF_MEMALLOC;

							/* 在各个管理区中，回收和释放干净的页面 */
							for (;;) {
								zone_t *z = *(zone++);
								if (!z)
									break;
								if (!z->size)
									continue;
								while (z->inactive_clean_pages) {
									struct page * page;
									/* Move one page to the free list. */
									page = reclaim_page(z);
									if (!page)
										break;
									__free_page(page);
									/* Try if the allocation succeeds. */
									page = rmqueue(z, order);
									if (page)
										return page;
								}
							}
						}
						/*
						 * When we arrive here, we are really tight on memory.
						 * （如果运行到这里了，说明内存是真的紧缺了）
						 * We wake up kswapd and sleep until kswapd wakes us
						 * up again. After that we loop back to the start.
						 *
						 * We have to do this because something else might eat
						 * the memory kswapd frees for us and we need to be
						 * reliable. Note that we don't loop back for higher
						 * order allocations since it is possible that kswapd
						 * simply cannot free a large enough contiguous area
						 * of memory *ever*.
						 */
						if ((gfp_mask & (__GFP_WAIT|__GFP_IO)) == (__GFP_WAIT|__GFP_IO)) {
							/*
							 * 唤醒kswapd进程回收页面
							 * 直到kswapd再次唤醒我们
							 * 被唤醒的时候，如果是单页的分配请求
							 * 我们就回到开始的位置
							 */
							wakeup_kswapd(1);
							memory_pressure++;
							if (!order)
								goto try_again;
						/*
						 * If __GFP_IO isn't set, we can't wait on kswapd because
						 * kswapd just might need some IO locks /we/ are holding ...
						 *
						 * SUBTLE: The scheduling point above makes sure that
						 * kswapd does get the chance to free memory we can't
						 * free ourselves...
						 */
						} else if (gfp_mask & __GFP_WAIT) {
							try_to_free_pages(gfp_mask);
							memory_pressure++;
							if (!order)
								goto try_again;
						}

					}

					/*
					 * Final phase: allocate anything we can!
					 * 最后的手段了，要是还分配不了内存，那肯定是系统问题了
					 */
					zone = zonelist->zones;
					for (;;) {
						zone_t *z = *(zone++);
						struct page * page = NULL;
						if (!z)
							break;
						if (!z->size)
							BUG();

						/*
						 * SUBTLE: direct_reclaim is only possible if the task
						 * becomes PF_MEMALLOC while looping above. This will
						 * happen when the OOM killer selects this task for
						 * instant execution...
						 */
						if (direct_reclaim) {
							page = reclaim_page(z);
							if (page)
								return page;
						}

						/* XXX: is pages_min/4 a good amount to reserve for this? */
						if (z->free_pages < z->pages_min / 4 &&
								!(current->flags & PF_MEMALLOC))
							continue;
						page = rmqueue(z, order);
						if (page)
							return page;
					}

					/* No luck.. */
					printk(KERN_ERR "__alloc_pages: %lu-order allocation failed.\n", order);
					return NULL;
				}

