/* Copyright (c) 2012, Bin Wei <bin@vip.qq.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * The name of of its contributors may not be used to endorse or 
 * promote products derived from this software without specific prior 
 * written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <config.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "coro_mm.h"
#include "mt_utils.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define BLOCK_TAG_1 0xaabb7788
#define BLOCK_TAG_2 0xccdd9900

struct block_info
{
	/* TAGs for error detection */
	volatile unsigned long tag1;
	struct block_info * volatile next;
	volatile unsigned long tag2;
};

struct page_info
{
	volatile unsigned int use_flag : 1;
	volatile unsigned int head_page : 31;
};
/* for 32bit atomic operation */
CASSERT(sizeof(struct page_info) == sizeof(uint32_t));

static int g_init_flag;
static size_t g_block_pages;
static size_t g_pool_blocks;
static size_t g_pool_pages;
static void *g_pool_addr;
static struct page_info *g_page_map;
static struct block_info * volatile g_free_list;
static MICORO_LOCK_T g_lock = MICORO_LOCK_INITVAL;
static struct coro_mm_stat g_stat;

#define IN_POOL(addr) ((void *)(addr) < (g_pool_addr + (g_pool_pages<<PAGE_SHIFT)) \
						&& (void *)(addr) >= g_pool_addr)
#define PAGE_NO(addr) ((size_t)(((void *)(addr) - g_pool_addr) >> PAGE_SHIFT))
#define NO_PTR(no) ((no) ? (void *)(g_pool_addr + ((size_t)(no) << PAGE_SHIFT)) : NULL)
#define PTR_HEAD(addr) (NO_PTR(g_page_map[PAGE_NO(addr)].head_page))

static inline size_t guard_pages(size_t blocks)
{
	/*
	 * garde-page(G) & block-pages(B) interleaved as:
	 *  <G B*1 G B*2 G B*4 G B*8 ... G>
	 *
	 *  blocks     guard-pages
	 * (00, 01] -> 1 +1
	 * (01, 03] -> 2 +1
	 * (03, 07] -> 3 +1
	 * (07, 0F] -> 4 +1
	 * ...
	 *
	 * Algo: effective_bit_width(blocks) + 1
	 */
	size_t i;
	for (i = 1; blocks &~ ((1UL<<i) - 1); i++);
	return (i + 1);
}

static int do_init_pool()
{
	size_t blk, guard = 0;
	struct block_info *cur = g_pool_addr, *prev = NULL;
	size_t i, blk_page_no;

	for (blk = 0; blk < g_pool_blocks; blk++) {

		/* insert guard page */
		if (blk == guard) {
#if HAVE_GOOD_MPROTECT
			if (mprotect(cur, PAGE_SIZE, PROT_NONE) < 0)
				return -1;
#endif
			/* page_no 0 means NULL */
			g_page_map[PAGE_NO(cur)].head_page = 0;

			cur = (void *)cur + PAGE_SIZE;
			guard = (guard << 1) + 1;
		}

		/* link empty block */
		cur->tag1 = BLOCK_TAG_1;
		cur->tag2 = BLOCK_TAG_2;
		cur->next = NULL;
		if (prev)
			prev->next = cur;
		else
			g_free_list = cur;
		prev = cur;

		blk_page_no = PAGE_NO(cur);
		for (i = 0; i < g_block_pages; i++) {
			g_page_map[blk_page_no + i].head_page = blk_page_no;
		}

		cur = (void *)cur + (g_block_pages << PAGE_SHIFT);
	}
	/* end by a guard-page */
#if HAVE_GOOD_MPROTECT
	if (mprotect(cur, PAGE_SIZE, PROT_NONE) < 0)
		return -1;
#endif
	g_page_map[PAGE_NO(cur)].head_page = 0;
	cur = (void *)cur + PAGE_SIZE;
	/* last check */
	assert(cur == g_pool_addr + (g_pool_pages << PAGE_SHIFT));

	return 0;
}

static int init(size_t *alloc_size, size_t pool_size)
{
	if (init_once(&g_init_flag) < 0) {
		fprintf(stderr, "init multi times!");
		return -1;
	}

	g_block_pages = (*alloc_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	g_pool_blocks = pool_size;
	if (g_block_pages <= 0 || g_pool_blocks <= 0)
		return -1;
	g_pool_pages = g_block_pages * g_pool_blocks + guard_pages(g_pool_blocks);

#ifndef MAP_ANONYMOUS
#	define MAP_ANONYMOUS MAP_ANON
#endif
	g_pool_addr = mmap(NULL, (g_pool_pages << PAGE_SHIFT), PROT_READ|PROT_WRITE,
			MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (g_pool_addr == MAP_FAILED) {
		perror("mmap failed");
		return -2;
	}

	g_page_map = calloc(g_pool_pages, sizeof(struct page_info));
	if (g_page_map == NULL) {
		perror("calloc failed");
		munmap(g_pool_addr, (g_pool_pages << PAGE_SHIFT));
		return -3;
	}

	if (do_init_pool() < 0) {
		perror("do_init_pool failed");
		free(g_page_map);
		munmap(g_pool_addr, (g_pool_pages << PAGE_SHIFT));
		return -4;
	}

	*alloc_size = g_block_pages << PAGE_SHIFT;
	return 0;
}

static void* alloc()
{
	struct block_info *block;

	MICORO_LOCK(&g_lock);
	block = g_free_list;
	if (block) {
		g_free_list = block->next;
		g_stat.alloc_count++;
		g_stat.use_block_num++;
	}
	MICORO_UNLOCK(&g_lock);

	if (!block) return NULL;

	assert(block->tag1 == BLOCK_TAG_1
			&& block->tag2 == BLOCK_TAG_2);
	assert(g_page_map[PAGE_NO(block)].use_flag == 0);

	g_page_map[PAGE_NO(block)].use_flag = 1;

	return block;
}

static int release(void *ptr)
{
	struct block_info *block = ptr;
	struct page_info pi;

	if (!IN_POOL(ptr) || PTR_HEAD(ptr) != ptr) {
		abort();
		return -1;
	}

	pi = g_page_map[PAGE_NO(ptr)];
	pi.use_flag = 0;
	ATOM_SWAP32((uint32_t*)&g_page_map[PAGE_NO(ptr)], (uint32_t*)&pi);
	if (pi.use_flag == 0) {
		abort();
		return -2;
	}

	MICORO_LOCK(&g_lock);
	/* within lock for mem concurrency */
	block->tag1 = BLOCK_TAG_1;
	block->tag2 = BLOCK_TAG_2;
	block->next = g_free_list;
	g_free_list = block;
	g_stat.release_count++;
	g_stat.use_block_num--;
	MICORO_UNLOCK(&g_lock);
	
	return 0;
}

static void* locate(void *ptr)
{
	if (!IN_POOL(ptr))
		return NULL;
	return PTR_HEAD(ptr);
}

static void get_stat(struct coro_mm_stat *stat)
{
	stat->alloc_count = g_stat.alloc_count;
	stat->release_count = g_stat.release_count;
	stat->use_block_num = g_stat.use_block_num;
	assert(g_pool_blocks >= g_stat.use_block_num);
	stat->free_block_num = g_pool_blocks - g_stat.use_block_num;
}

static struct coro_mm_ops g_ops = {
	.init = init,
	.alloc = alloc,
	.release = release,
	.locate = locate,
	.get_stat = get_stat
};

struct coro_mm_ops *coro_mm_pool_ops = &g_ops;

