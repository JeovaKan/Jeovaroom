/*
 * 页面定期换出
 */

   为了避免发生缺页异常时，再通过搜寻来获取可用物理页面的情况

   内核定期检查并且预先将若干页面换出，腾出空间

   每隔多久换出一次，每次换出多少个页面？

   使得发生缺页异常时必须临时寻找页面换出的情况，实际上很少发生

   -->kswapd

   首先系统初始化期间创建这个进程

   mm/vmscan.c--> kswapd_init()

	   static int __init kswapd_init(void)
		{
			printk("Starting kswapd v1.8\n");

			/* 根据物理内存大小设定一个全局变量 page_cluster */
			swap_setup();

			/* 创建一个内核线程 */
			kernel_thread(kswapd, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);
			kernel_thread(kreclaimd, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);
			return 0;
		}

		/*
		 * 设定的全局量和磁盘设备驱动有关
		 * ：预读的页面的数量
		 */
		kswapd_init-->swap_setup()

			void __init swap_setup(void)
			{
				/* Use a smaller cluster for memory <16MB or <32MB */
				if (num_physpages < ((16 * 1024 * 1024) >> PAGE_SHIFT))
					page_cluster = 2;
				else if (num_physpages < ((32 * 1024 * 1024) >> PAGE_SHIFT))
					page_cluster = 3;
				else
					page_cluster = 4;
			}

	跳过内核进程创建的过程，先看看kswapd做了什么

	mm/vmscan.c--> kswapd()

	int kswapd(void *unused)
	{
		struct task_struct *tsk = current;

		tsk->session = 1;
		tsk->pgrp = 1;
		strcpy(tsk->comm, "kswapd");
		sigfillset(&tsk->blocked);
		kswapd_task = tsk;

		tsk->flags |= PF_MEMALLOC;

		/*
		 * Kswapd main loop.
		 */
		for (;;) {
			static int recalc = 0;

			/* 检查内存中可供分配和周转的物理页面是否短缺 */
			if (inactive_shortage() || free_shortage()) {
				int wait = 0;
				/* 看一看kswapd_done队列中有没有在排队的调用 */
				if (waitqueue_active(&kswapd_done))
					wait = 1;
				/* 如果确实存在短缺的情况，那么尝试释放一些页面 */
				do_try_to_free_pages(GFP_KSWAPD, wait);
			}

			/*
			 * 经过do_try_to_free_pages的工作
			 * 活跃页面的队列可能已经有变化
			 * 再次调用refill_inactive_scan()
			 */
			refill_inactive_scan(6, 0);

			/*
			 * 根据当前系统的节拍
			 * 定期执行recalculate_vm_stats
			 */
			if (time_after(jiffies, recalc + HZ)) {
				recalc = jiffies;
				recalculate_vm_stats();
			}

			/*
			 * 由于kswapd可能是被其他进程唤醒的
			 * 那么其执行后要唤醒等待其完成的
			 * 睡眠进程
			 */
			wake_up_all(&kswapd_done);
			run_task_queue(&tq_disk);

			/* 
			 * 当不再有页面短缺的时候，kswapd就进入睡眠了
			 * 1. 系统自动1s唤醒一次
			 * 2. 被其他进程唤醒
			 */
			if (!free_shortage() || !inactive_shortage()) {
				interruptible_sleep_on_timeout(&kswapd_wait, HZ);
			/*
			 * 如果是由于系统可用内存确实不够了
			 * 就杀掉一些进程来获得页面
			 */
			} else if (out_of_memory()) {
				oom_kill();
			}
		}
	}

		kswapd-->inactive_shortage
		/*
		 * 系统中应该维持物理页面供应量由两个全局量确定
		 * freepages.high：空闲页面的数量
		 * inactive_target:不活跃页面的数量
		 * 二者之和为正常情况下的潜在供应量
		 */
		int inactive_shortage(void)
		{
			int shortage = 0;

			shortage += freepages.high;
			shortage += inactive_target;
			shortage -= nr_free_pages(); //当前空闲的页面
			shortage -= nr_inactive_clean_pages(); //当前不活跃干净的页面
			shortage -= nr_inactive_dirty_pages; //当前不活跃脏的页面

			if (shortage > 0)
				return shortage;

			return 0;
		}

		kswapd-->free_shortage
		/*
		 * 统计各个ZONE区的可分配页面是否
		 * 低于最低量
		 */
		int free_shortage(void)
		{
			pg_data_t *pgdat = pgdat_list;
			int sum = 0;
			int freeable = nr_free_pages() + nr_inactive_clean_pages();
			int freetarget = freepages.high + inactive_target / 3;

			/* 是否全局页面短缺的情况 */
			if (freeable < freetarget)
				return freetarget - freeable;

			/* 如果不是，是否每个区的可分配情况低于最低值 */
			do {
				int i;
				for(i = 0; i < MAX_NR_ZONES; i++) {
					zone_t *zone = pgdat->node_zones+ i;
					if (zone->size && (zone->inactive_clean_pages +
							zone->free_pages < zone->pages_min+1)) {
						/* + 1 to have overlap with alloc_pages() !! */
						sum += zone->pages_min + 1;
						sum -= zone->free_pages;
						sum -= zone->inactive_clean_pages;
					}
				}
				pgdat = pgdat->node_next;
			} while (pgdat);

			return sum;
		}


		kswapd-->waitqueue_active()
		/*
		 * 在真正执行释放页面的调用之前
		 * 先看看kswapd_done队列中是否有函数在等待执行
		 * 内核中有几个特殊的队列，内核中的各个部分（主要是设备驱动）
		 * 可以把一些低层函数(???)挂入这样的队列，使得这些函数在某种
		 * 事件发生的时候就能得到执行
		 */
		static inline int waitqueue_active(wait_queue_head_t *q)
		{
		#if WAITQUEUE_DEBUG
			if (!q)
				WQ_BUG();
			CHECK_MAGIC_WQHEAD(q);
		#endif

			return !list_empty(&q->task_list);
		}

		kswapd-->do_try_to_free_pages()
		/*
		 * 前述的检查都已经做完了，此时我们尝试释放一些页面
		 */
		static int do_try_to_free_pages(unsigned int gfp_mask, int user)
		{
			int ret = 0;

			/*
			 * 先易后难，做到不影响现役的页面
			 * 那么先考虑不活跃脏的页面，将他们洗干净
			 * 放回干净队列
			 */
			if (free_shortage() || nr_inactive_dirty_pages > nr_free_pages() +
					nr_inactive_clean_pages())
				ret += page_launder(gfp_mask, user);

			/*
			 * 如果经过了脏页面的清洗，仍然页面短缺的话
			 * 就采用以下三个操作了
			 */
			if (free_shortage() || inactive_shortage()) {
				/*
				 * 前两个操作与文件系统中使用到的dentry
				 * 和inode结构有关，缓存这两个数据结构是为了
				 * 防止反复地访问之前路径下的目录、文件。
				 * 此时通过前两个调用适当地加以回收
				 */
				shrink_dcache_memory(6, gfp_mask);
				shrink_icache_memory(6, gfp_mask);
				ret += refill_inactive(gfp_mask, user);
			} else {
				/*
				 * 每隔一段时间，就通过这个调用来回收slab
				 * slab是一种将整块的内存分割零售的机制
				 */
				kmem_cache_reap(gfp_mask);
				ret = 1;
			}

			return ret;
		}

		kswapd-->do_try_to_free_pages-->page_launder()

			int page_launder(int gfp_mask, int sync)
			{
				/*
				 * launder_loop：控制扫描不活跃脏页面队列的次数
				                 当第二次扫描的时候，回到标号dirty_page_rescan的位置
				 * cleaned_pages：累计被清洗的页面数量
				 * maxscan：循环中会把某些页面从当前位置移动到
                            队列的尾部，加以数量控制，避免重复处理同一个页面。
				 */
				int launder_loop, maxscan, cleaned_pages, maxlaunder;
				int can_get_io_locks;
				struct list_head * page_lru;
				struct page * page;

				/*
				 * We can only grab the IO locks (eg. for flushing dirty
				 * buffers to disk) if __GFP_IO is set.
				 */
				can_get_io_locks = gfp_mask & __GFP_IO;

				launder_loop = 0;
				maxlaunder = 0;
				cleaned_pages = 0;

			dirty_page_rescan:
				spin_lock(&pagemap_lru_lock);
				maxscan = nr_inactive_dirty_pages;
				while ((page_lru = inactive_dirty_list.prev) != &inactive_dirty_list &&
							maxscan-- > 0) {
					page = list_entry(page_lru, struct page, lru);

					/*
					 * 先检查页面的状态，如果不是脏的
					 * 那一定是出现了什么错误，所以将它从队列中删除 
					 */
					if (!PageInactiveDirty(page)) {
						printk("VM: page_launder, wrong page on list.\n");
						list_del(page_lru);
						nr_inactive_dirty_pages--;
						page->zone->inactive_dirty_pages--;
						continue;
					}

					/*
					 * 1. 页面在进入了不活跃脏页面队列之后又受到了访问
					      又发生了缺页异常，恢复了这个页面的映射
					 * 2. 页面的寿命还没有耗尽，page结构中有一个字段是age
					      其数值与页面受访问的频繁程度有关
					 * 3. 页面没有用作读/写缓冲，但是页面的使用计数>1，
					      说明至少有一个进程在使用这个页面

					 * 4. 如果页面用作内存模拟磁盘，那么是不能被换出的
 					 */
					if (PageTestandClearReferenced(page) || page->age > 0 ||
							(!page->buffers && page_count(page) > 1) ||
							page_ramdisk(page)) {
						del_page_from_inactive_dirty_list(page);
						add_page_to_active_list(page);
						continue;
					}

					/*
					 * 如果页面是锁定的，说明当前正在对此页面进行操作
					 * 把这个页面移动到当前队列的末尾
					 */
					if (TryLockPage(page)) {
						list_del(page_lru);
						list_add(page_lru, &inactive_dirty_list);
						continue;
					}

					if (PageDirty(page)) {
						int (*writepage)(struct page *) = page->mapping->a_ops->writepage;
						int result;

						/*
						 * 如果没有写操作函数
						 * 就把页面送回到活跃页面队列里面
						 * 也就是说页面必须要是可以物理写回的 
						 */
						if (!writepage)
							goto page_active;

						/*
						 * 如果是在第一次扫描中，那么只将页面放入到队列的末尾中 
						 */
						if (!launder_loop) {
							list_del(page_lru);
							list_add(page_lru, &inactive_dirty_list);
							UnlockPage(page);
							continue;
						}

						/*
						 * 1. 清除dirty标志
						 * 2. 增加页面的使用者
						 * 3. 解锁，通过ops里面的写函数，回写页面
						 * 4. 减少页面的使用者
						 */
						ClearPageDirty(page);
						page_cache_get(page);
						spin_unlock(&pagemap_lru_lock);

						result = writepage(page);
						page_cache_release(page);

						spin_lock(&pagemap_lru_lock);
						if (result != 1)
							continue;
						/*
						 * 如果写出失败了，那么就再次设置页面脏
						 * 的标志，并将页面退还到活跃队列中，
						 * 毕竟本次操作失败了呢
						 */
						set_page_dirty(page);
						goto page_active;
					}

					/*
					 * 如果页面是用做读/写缓冲的
					 */
					if (page->buffers) {
						int wait, clearedbuf;
						int freed_page = 0;
						/*
						 * Since we might be doing disk IO, we have to
						 * drop the spinlock and take an extra reference
						 * on the page so it doesn't go away from under us.
						 */
						del_page_from_inactive_dirty_list(page);
						page_cache_get(page);
						spin_unlock(&pagemap_lru_lock);

						/* Will we do (asynchronous) IO? */
						if (launder_loop && maxlaunder == 0 && sync)
							wait = 2;	/* Synchrounous IO */
						else if (launder_loop && maxlaunder-- > 0)
							wait = 1;	/* Async IO */
						else
							wait = 0;	/* No IO */

						/* Try to free the page buffers. */
						clearedbuf = try_to_free_buffers(page, wait);

						/*
						 * Re-take the spinlock. Note that we cannot
						 * unlock the page yet since we're still
						 * accessing the page_struct here...
						 */
						spin_lock(&pagemap_lru_lock);

						/* 如果缓冲区没有成功释放，那么页面放回脏的队列 */
						if (!clearedbuf) {
							add_page_to_inactive_dirty_list(page);

						/* 如果当前页不在任何队列中？只在缓冲区中 */
						} else if (!page->mapping) {
							atomic_dec(&buffermem_pages);
							freed_page = 1;
							cleaned_pages++;

						/* 当前页面的用户不止我们一个 */
						} else if (page_count(page) > 2) {
							add_page_to_active_list(page);

						/* 运行到这的话，是一个可以释放的页面了，把它加入不活跃队列 */
						} else /* page->mapping && page_count(page) == 2 */ {
							add_page_to_inactive_clean_list(page);
							cleaned_pages++;
						}

						/*
						 * Unlock the page and drop the extra reference.
						 * We can only do it here because we ar accessing
						 * the page struct above.
						 */
						UnlockPage(page);
						page_cache_release(page);

						/* 
						 * If we're freeing buffer cache pages, stop when
						 * we've got enough free memory.
						 */
						if (freed_page && !free_shortage())
							break;
						continue;
					} else if (page->mapping && !PageDirty(page)) {
						/*
						 * If a page had an extra reference in
						 * deactivate_page(), we will find it here.
						 * Now the page is really freeable, so we
						 * move it to the inactive_clean list.
						 */
						del_page_from_inactive_dirty_list(page);
						add_page_to_inactive_clean_list(page);
						UnlockPage(page);
						cleaned_pages++;
					} else {
			page_active:
						/*
						 * 释放的过程中遇到了问题
						 * 或者说是目前无法释放的页面
						 * 都将它放回活跃的队列里面
						 */
						del_page_from_inactive_dirty_list(page);
						add_page_to_active_list(page);
						UnlockPage(page);
					}
				}
				spin_unlock(&pagemap_lru_lock);

				/*
				 * If we don't have enough free pages, we loop back once
				 * to queue the dirty pages for writeout. When we were called
				 * by a user process (that /needs/ a free page) and we didn't
				 * free anything yet, we wait synchronously on the writeout of
				 * MAX_SYNC_LAUNDER pages.
				 *
				 * We also wake up bdflush, since bdflush should, under most
				 * loads, flush out the dirty pages before we have to wait on
				 * IO.
				 */
				if (can_get_io_locks && !launder_loop && free_shortage()) {
					/*
					 * launder_loop设置为1以后，可以再执行一次搜索
					 * 但是只能再执行一次(???)
					 * page_launder只执行两次
					 */
					launder_loop = 1;
					/* If we cleaned pages, never do synchronous IO. */
					if (cleaned_pages)
						sync = 0;
					/* We only do a few "out of order" flushes. */
					maxlaunder = MAX_LAUNDER;
					/* Kflushd takes care of the rest. */
					wakeup_bdflush(0);
					goto dirty_page_rescan;
				}

				/* Return the number of pages moved to the inactive_clean list. */
				return cleaned_pages;
			}

		-->这一段工作后我们返回到do_try_to_free_pages()


		/*
		 * 如果洗脏页的操作仍然不能满足需求
		 * 就通过其他的4个手段来获取了
		 * 此处先观察，refill_inactive()
		 */

		kswapd-->do_try_to_free_pages-->refill_inactive()
		/*
		 * user : 上层传入的参数，表示是否有函数在kswapd_done队列中等待
		 * gfp_mask: 
		 */
		static int refill_inactive(unsigned int gfp_mask, int user)
		{
			int priority, count, start_count, made_progress;

			count = inactive_shortage() + free_shortage();
			if (user)
				count = (1 << page_cluster);
			start_count = count;

			/* 首先通过收割slab区，相对的操作最小 */
			kmem_cache_reap(gfp_mask);

			/* 代表优先级，首先从最低的6级开始 */
			priority = 6;
			do {
				made_progress = 0;

				/* 
				 * 检查当前进程的 need_resched
				 * 如果为1，说明某个中断服务程序要求调度
				 * 所以要调用schedule()让内核进行一次调度
				 * 预先将本进程设置为TASK_RUNNING，表达继续运行的意愿
				 */
				if (current->need_resched) {
					__set_current_state(TASK_RUNNING);
					schedule();
				}

				/*
				 * 循环扫描活跃队列，试图找到可以转入活跃队列的页面
				 */
				while (refill_inactive_scan(priority, 1)) {
					made_progress = 1;
					if (--count <= 0)
						goto done;
				}

				/*
				 * 扫描inode和dentry结构
				 */
				shrink_dcache_memory(priority, gfp_mask);
				shrink_icache_memory(priority, gfp_mask);

				/*
				 * 找出一个进程，尝试从其映射表中找到到可以转入不活跃队列的页表
				 */
				while (swap_out(priority, gfp_mask)) {
					made_progress = 1;
					if (--count <= 0)
						goto done;
				}

				/*
				 * If we either have enough free memory, or if
				 * page_launder() will be able to make enough
				 * free memory, then stop.
				 */
				if (!inactive_shortage() || !free_shortage())
					goto done;

				/*
				 * Only switch to a lower "priority" if we
				 * didn't make any useful progress in the
				 * last loop.
				 */
				if (!made_progress)
					priority--;
			} while (priority >= 0);

			/* Always end on a refill_inactive.., may sleep... */
			while (refill_inactive_scan(0, 1)) {
				if (--count <= 0)
					goto done;
			}

		done:
			return (count < start_count);
		}

			kswapd-->do_try_to_free_pages->refill_inactive-->refill_inactive_scan()

			int refill_inactive_scan(unsigned int priority, int oneshot)
			{
				struct list_head * page_lru;
				struct page * page;
				int maxscan, page_active = 0;
				int ret = 0;

				/* Take the lock while messing with the list... */
				spin_lock(&pagemap_lru_lock);

				/* 根据输入的优先级，决定当前扫描的最大页面数量 */
				maxscan = nr_active_pages >> priority;
				while (maxscan-- > 0 && (page_lru = active_list.prev) != &active_list) {
					page = list_entry(page_lru, struct page, lru);

					/* 检查当前页面确实属于活动的页面 */
					if (!PageActive(page)) {
						printk("VM: refill_inactive, wrong page on list.\n");
						list_del(page_lru);
						nr_active_pages--;
						continue;
					}

					/*
					 * 检查该页面是否受到了访问，清除并返回原值
					 */
					if (PageTestandClearReferenced(page)) {
						/*
						 * 如果寿命为0,将页面转入活跃队列
						 * 增加寿命值#define PAGE_AGE_ADV 3
						 */
						age_page_up_nolock(page);
						page_active = 1;
					} else {
						
						/* 没有受到访问的页面，减小寿命 */
						age_page_down_ageonly(page);
						
						/*
						 * 如果寿命为0，需要考虑是否仍有用户映射
						 * 对于不作为文件读/写缓冲的页面，只有值不是
						 * 0，说明都有映射存在。那么就要将页面放回到
						 * 不活跃脏队列中（有最大映射数量的条件）
						 */
						if (page->age == 0 && page_count(page) <=
									(page->buffers ? 2 : 1)) {
							deactivate_page_nolock(page);
							page_active = 0;
						} else {
							page_active = 1;
						}
					}
					/*
					 * 通过以上的筛选，仍然是可活跃的页面就被转入
					 * 活跃队列了，否则判断oneshot的值，决定是不是要再
					 * 循环一次
					 */
					if (page_active || PageActive(page)) {
						list_del(page_lru);
						list_add(page_lru, &active_list);
					} else {
						ret = 1;
						if (oneshot)
							break;
					}
				}
				spin_unlock(&pagemap_lru_lock);

				return ret;
			}

			kswapd-->do_try_to_free_pages->refill_inactive-->refill_inactive_scan-->age_page_up_nolock()

			void age_page_up_nolock(struct page * page)
			{
				/*
				 * 如果页面的寿命已经到了0
				 * 将它从原有队列移动到活跃队列
				 */
				if (!page->age)
					activate_page_nolock(page);

				/* The actual page aging bit */
				page->age += PAGE_AGE_ADV;
				if (page->age > PAGE_AGE_MAX)
					page->age = PAGE_AGE_MAX;
			}

		kswapd-->do_try_to_free_pages-->swap_out()
		
		#define SWAP_SHIFT 5
		#define SWAP_MIN 8

		static int swap_out(unsigned int priority, int gfp_mask)
		{
			int counter;
			int __ret = 0;

			/* 
			 * counter值决定了第一层for循环的次数
			 * 根据系统中的进程（包括线程）的个数以及传入的优先级
			 * 计算得到当前循环的次数
			 */
			counter = (nr_threads << SWAP_SHIFT) >> priority;
			if (counter < 1)
				counter = 1;

			/*
			 * 每一次循环中，找到最适合的那个进程best
			 * 将其页面表中符合条件的页面交换出去 
			 */
			for (; counter >= 0; counter--) {
				struct list_head *p;
				unsigned long max_cnt = 0;
				struct mm_struct *best = NULL;
				int assign = 0;
				int found_task = 0;
			select:
				spin_lock(&mmlist_lock);
				p = init_mm.mmlist.next;
				for (; p != &init_mm.mmlist; p = p->next) {
					struct mm_struct *mm = list_entry(p, struct mm_struct, mmlist);
					/*
					 * 每个进程都有自己的虚拟空间
					 * 空间中已经分配并建立了映射的页面构成了一个集合
					 * 在任意一个给定的时刻，该集合中的每一个页面对应的
					 * 物理页面不一定都在内存中，在内存中的是它的一个子集
					 * 称为resident set，大小称为rss
					 */
			 		if (mm->rss <= 0)
						continue;
					found_task++;
					/* Refresh swap_cnt? */
					if (assign == 1) {
						/*
						 * 反映了进程在一轮换出页面的检查中
						 * 尚未受到考察的页面数量
						 */
						mm->swap_cnt = (mm->rss >> SWAP_SHIFT);
						if (mm->swap_cnt < SWAP_MIN)
							mm->swap_cnt = SWAP_MIN;
					}
					/* 进程中找到swap_cnt最大进程 */
					if (mm->swap_cnt > max_cnt) {
						max_cnt = mm->swap_cnt;
						best = mm;
					}
				}

				/* Make sure it doesn't disappear */
				if (best)
					atomic_inc(&best->mm_users);
				spin_unlock(&mmlist_lock);

				/*
				 * We have dropped the tasklist_lock, but we
				 * know that "mm" still exists: we are running
				 * with the big kernel lock, and exit_mm()
				 * cannot race with us.
				 */
				if (!best) {
					if (!assign && found_task > 0) {
						assign = 1;
						goto select;
					}
					break;
				} else {
					__ret = swap_out_mm(best, gfp_mask);
					mmput(best);
					break;
				}
			}
			return __ret;
		}

			kswapd-->do_try_to_free_pages-->swap_out-->swap_out_mm()
			/*
			 * 如果找到了可以换出的页面
			 * 那么就开始尝试一级一级的释放这个页面
			 * 从swap_out_mm开始，swap_out_vma->swap_out_pgd->swap_out_pmd->
			 * try_to_swap_out()
			 */
			static int swap_out_mm(struct mm_struct * mm, int gfp_mask)
			{
				int result = 0;
				unsigned long address;
				struct vm_area_struct* vma;

				spin_lock(&mm->page_table_lock);
				address = mm->swap_address;
				vma = find_vma(mm, address);
				if (vma) {
					if (address < vma->vm_start)
						address = vma->vm_start;

					for (;;) {
						result = swap_out_vma(mm, vma, address, gfp_mask);
						if (result)
							goto out_unlock;
						vma = vma->vm_next;
						if (!vma)
							break;
						address = vma->vm_start;
					}
				}
				/* Reset to 0 when we reach the end of address space */
				mm->swap_address = 0;
				mm->swap_cnt = 0;

			out_unlock:
				spin_unlock(&mm->page_table_lock);
				return result;
			}

				kswapd-->do_try_to_free_pages-->swap_out-->swap_out_mm->swap_out_vma->swap_out_pgd->swap_out_pmd->try_to_swap_out()
				/*
				 * 中间几步对于pgd、pmd的操作类似
				 * 这里跳至最后的关键一步
				 */
				static int try_to_swap_out(struct mm_struct * mm, struct vm_area_struct* vma, unsigned long address, pte_t * page_table, int gfp_mask)
				{
					pte_t pte;
					swp_entry_t entry;
					struct page * page;
					int onlist;

					/*
					 * 首先检查传入的找到页面的pte表项的值
					 * 如果不满足要求，此次换出失败
					 */
					pte = *page_table;
					if (!pte_present(pte))
						goto out_failed;
					/*
					 * 页表是否在合理的范围内，是否还驻存在内存中？
					 * 如果不满足要求，此次换出失败
					 */
					page = pte_page(pte);
					if ((!VALID_PAGE(page)) || PageReserved(page))
						goto out_failed;

					if (!mm->swap_cnt)
						return 1;

					/* swap_cnt - 1，理由我还不知道 */
					mm->swap_cnt--;

					/*
					 * 检查页面是否在活跃队列中？
					 */
					onlist = PageActive(page);
					/*
					 * 页面表项中有个标志_PAGE_ACCESSED
					 * 访问这个物理地址的时候，就是设置这个标志为1
					 * 从上一次对同一个页面表项调用try_to_swap_out至今，
					 * 该页面至少被访问过一次，说明这个页面还年轻
					 * 可能被再次访问。此次清除标志，为下一次检查这个
					 * 页面做准备
					 */
					if (ptep_test_and_clear_young(page_table)) {
						age_page_up(page);
						goto out_failed;
					}
					if (!onlist)
						/* The page is still mapped, so it can't be freeable... */
						age_page_down_ageonly(page);

					/*
					 * If the page is in active use by us, or if the page
					 * is in active use by others, don't unmap it or
					 * (worse) start unneeded IO.
					 */
					if (page->age > 0)
						goto out_failed;

					/*
					 * 后续对于Page数据结构的操作涉及互斥
					 * 需要对数据结构加锁，如果这个锁已经被使用
					 * 返回值1，说明此时不能对页面操作
					 */
					if (TryLockPage(page))
						goto out_failed;

					/* 
					 * 再一次获得页表项，并清除内容
					 * 暂时停止表项的映射，在多处理器系统中，目标进程可能在
					 * 另一个CPU上运行，映射的内容也可能改变
					 */
					pte = ptep_get_and_clear(page_table);
					flush_tlb_page(vma, address);

					/*
					 * 如果当前页面已经在换入/换出的队列中（PageSwapCache检测swap标志是否设置）
					 * 此时page结构位于swapper_space队列中
					 * 页面已经在交换设备上，把映射断开即可
					 * 如果已经受到过写的访问，就放入脏的队列
					 */
					if (PageSwapCache(page)) {
						/*
						 * 在swapper_space队列中的page结构
						 * 其index字段保存的是32位的索引项
						 * 是指向页面在交换设备上的映像的指引
						 */
						entry.val = page->index;
						if (pte_dirty(pte))
							set_page_dirty(page);
				set_swap_pte:
						swap_duplicate(entry);
						/*
						 * 将指向盘上的索引设置为页面表项
						 * 此时对内存页面的映射就变成了对盘上页面的映射
						 */
						set_pte(page_table, swp_entry_to_pte(entry));
				drop_pte:
						UnlockPage(page);
						mm->rss--;
						/*
						 * 经过上述换出操作，这个页面很可能已经满足不活跃的状态
						 * 那么就将它转移到某个不活跃队列中
						 */
						deactivate_page(page);
						page_cache_release(page);
				out_failed:
						return 0;
					}

					/*
					 * Is it a clean page? Then it must be recoverable
					 * by just paging it in again, and we can just drop
					 * it..
					 *
					 * However, this won't actually free any real
					 * memory, as the page will just be in the page cache
					 * somewhere, and as such we should just continue
					 * our scan.
					 *
					 * Basically, this just makes it possible for us to do
					 * some real work in the future in "refill_inactive()".
					 */
					flush_cache_page(vma, address);
					if (!pte_dirty(pte))
						goto drop_pte;

					/*
					 * Ok, it's really dirty. That means that
					 * we should either create a new swap cache
					 * entry for it, or we should write it back
					 * to its own backing store.
					 */
					if (page->mapping) {
						set_page_dirty(page);
						goto drop_pte;
					}

					/*
					 * This is a dirty, swappable page.  First of all,
					 * get a suitable swap entry for it, and make sure
					 * we have the swap cache set up to associate the
					 * page with that swap entry.
					 */
					entry = get_swap_page();
					if (!entry.val)
						goto out_unlock_restore; /* No swap space left */

					/* Add it to the swap cache and mark it dirty */
					add_to_swap_cache(page, entry);
					set_page_dirty(page);
					goto set_swap_pte;

				out_unlock_restore:
					set_pte(page_table, pte);
					UnlockPage(page);
					return 0;
				}

					kswapd-->do_try_to_free_pages-->swap_out-->swap_out_mm->swap_out_vma->swap_out_pgd->swap_out_pmd->try_to_swap_out-->swap_duplicate()

					/*
					 * 对索引到的内容做一些检验，同时增加页面共享计数
					 * swp_entry_t 实际上是一个32位无符号整数
					 * 高24位offset为设备上的页面序号，其余7个位代表交换设备本身的序号
					 * 最低位始终为0
					 */
					int swap_duplicate(swp_entry_t entry)
					{
						struct swap_info_struct * p;
						unsigned long offset, type;
						int result = 0;

						/* Swap entry 0 is illegal */
						if (!entry.val)
							goto out;
						/*
						 * 名称虽然是type
						 * 实际上代表的是交换设备本身的序号
						 * 以type为下标，就可以在swap_info中找到对应设备的swap_info_struct结构
						 * 数据结构中的swap_map数组记录交换设备上各个页面的共享计数
						 */
						type = SWP_TYPE(entry);
						if (type >= nr_swapfiles)
							goto bad_file;
						p = type + swap_info;
						offset = SWP_OFFSET(entry);
						if (offset >= p->max)
							goto bad_offset;
						/*
						 * 共享计数为0，说明是不正确的引用
						 */
						if (!p->swap_map[offset])
							goto bad_unused;
						/*
						 * Entry is valid, so increment the map count.
						 */
						swap_device_lock(p);
						if (p->swap_map[offset] < SWAP_MAP_MAX)
							p->swap_map[offset]++;
						else {
							static int overflow = 0;
							if (overflow++ < 5)
								printk("VM: swap entry overflow\n");
							p->swap_map[offset] = SWAP_MAP_MAX;
						}
						swap_device_unlock(p);
						result = 1;
					out:
						return result;

					bad_file:
						printk("Bad swap file entry %08lx\n", entry.val);
						goto out;
					bad_offset:
						printk("Bad swap offset entry %08lx\n", entry.val);
						goto out;
					bad_unused:
						printk("Unused swap offset entry in swap_dup %08lx\n", entry.val);
						goto out;
					}

					kswapd-->do_try_to_free_pages-->swap_out-->swap_out_mm->swap_out_vma->swap_out_pgd->swap_out_pmd->try_to_swap_out-->deactivate_page()

					void deactivate_page(struct page * page)
					{
						spin_lock(&pagemap_lru_lock);
						deactivate_page_nolock(page);
						spin_unlock(&pagemap_lru_lock);
					}

					deactivate_page-->deactivate_page_nolock()
						void deactivate_page_nolock(struct page * page)
						{
							/*
							 * 1. page_count计数值，在空闲页面时为0
							 * 2. 当分配这个页面的时候，将计数值设为1
							 * 3. 每当页面增加一个用户时，建立或者恢复映射时，计数值加1
							 * 4. 如果页面通过mmap()映射到一般文件，文件又已经被打开
							      页面同时用作文件的读/写缓冲区，page->buffers队列是这个
							      页面的又一个用户，所以当buffers不为0时，maxcount为3
							 */
							int maxcount = (page->buffers ? 3 : 2);
							page->age = 0;
							ClearPageReferenced(page);

							/*
							 * 同时满足：
							 * 1. 不活跃
							 * 2. page_count计数值
							 * 3. 不是用作内存页面模拟磁盘
							 */
							if (PageActive(page) && page_count(page) <= maxcount && !page_ramdisk(page)) {
								del_page_from_active_list(page);
								add_page_to_inactive_dirty_list(page);
							}
						}