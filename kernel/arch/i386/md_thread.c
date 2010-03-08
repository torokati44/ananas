#include "i386/thread.h"
#include "i386/vm.h"
#include "i386/macro.h"
#include "i386/smp.h"
#include "i386/realmode.h"
#include "lib.h"
#include "mm.h"
#include "options.h"
#include "pcpu.h"
#include "param.h"
#include "thread.h"
#include "vm.h"

extern struct TSS kernel_tss;

int
md_thread_init(thread_t thread)
{
	/* Note that this function relies on thread->md being zero-filled before calling */
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;

	/* Create a pagedirectory and map the kernel pages in there */
	md->pagedir = kmalloc(PAGE_SIZE);
	memset(md->pagedir, 0, PAGE_SIZE);
	vm_map_kernel_addr(md->pagedir);

	/* Allocate stacks: one for the thread and one for the kernel */
	md->stack  = kmalloc(THREAD_STACK_SIZE);
	md->kstack = kmalloc(KERNEL_STACK_SIZE);

	/* Perform adequate mapping for the stack / code */
	vm_map_pagedir(md->pagedir, (addr_t)md->stack,  THREAD_STACK_SIZE / PAGE_SIZE, 1);
	vm_map_pagedir(md->pagedir, (addr_t)md->kstack, KERNEL_STACK_SIZE / PAGE_SIZE, 0);

#ifdef SMP	
	/*
	 * Grr - for some odd reason, the GDT had to be subject to paging. This means
	 * we have to insert a suitable mapping for every CPU... :-/
	 */
	uint32_t i;
	for (i = 0; i < get_num_cpus(); i++) {
		struct IA32_CPU* cpu = get_cpu_struct(i);
		vm_map_pagedir(md->pagedir, (addr_t)cpu->gdt, 1 /* XXX */, 0);
	}
#endif

	/* Fill out the thread's registers - anything not here will be zero */ 
	md->ctx.esp  = (addr_t)md->stack  + THREAD_STACK_SIZE;
	md->ctx.esp0 = (addr_t)md->kstack + KERNEL_STACK_SIZE;
	md->ctx.cs = GDT_SEL_USER_CODE + SEG_DPL_USER;
	md->ctx.ds = GDT_SEL_USER_DATA;
	md->ctx.es = GDT_SEL_USER_DATA;
	md->ctx.ss = GDT_SEL_USER_DATA + SEG_DPL_USER;
	md->ctx.cr3 = (addr_t)md->pagedir;
	md->ctx.eflags = EFLAGS_IF;

	thread->next_mapping = 1048576;
	return 1;
}

size_t
md_thread_get_privdata_length()
{
	return sizeof(struct MD_THREAD);
}

void
md_thread_destroy(thread_t thread)
{
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;

	kfree(md->pagedir);
	kfree(md->stack);
	kfree(md->kstack);
}

void
md_thread_switch(thread_t new, thread_t old)
{
	struct MD_THREAD* md_new = (struct MD_THREAD*)new->md;
	struct CONTEXT* ctx_new = (struct CONTEXT*)&md_new->ctx;

	/*
	 * Activate this context as the current CPU context. XXX lock
	 */
	__asm(
		"movw %%bx, %%fs\n"
		"movl	%%eax, %%fs:0\n"
	: : "a" (ctx_new), "b" (GDT_SEL_KERNEL_PCPU));

	/* Fetch kernel TSS */
	struct TSS* tss;
	__asm(
		"movl	%%fs:8, %0\n"
	: "=r" (tss));

	/* Activate the corresponding kernel stack in the TSS */
	tss->esp0 = ctx_new->esp0;

	/* Go! */
	md_restore_ctx(ctx_new);
}

void*
md_map_thread_memory(thread_t thread, void* ptr, size_t length, int write)
{
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;
	KASSERT(length <= PAGE_SIZE, "no support for >PAGE_SIZE mappings yet!");

	addr_t addr = (addr_t)ptr & ~(PAGE_SIZE - 1);
	addr_t phys = vm_get_phys(md->pagedir, addr, write);
	if (phys == 0)
		return NULL;

	addr_t virt = TEMP_USERLAND_ADDR + PCPU_GET(cpuid) * TEMP_USERLAND_SIZE;
	vm_mapto(virt, phys, 2 /* XXX */);
	return (void*)virt + ((addr_t)ptr % PAGE_SIZE);
}

void*
md_thread_map(thread_t thread, void* to, void* from, size_t length, int flags)
{
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;
	int num_pages = length / PAGE_SIZE;
	if (length % PAGE_SIZE > 0)
		num_pages++;
	/* XXX cannot specify flags yet */
	vm_mapto_pagedir(md->pagedir, (addr_t)to, (addr_t)from, num_pages, 1);
	return to;
}

int
md_thread_unmap(thread_t thread, void* addr, size_t length)
{
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;

	int num_pages = length / PAGE_SIZE;
	if (length % PAGE_SIZE > 0)
		num_pages++;
	vm_unmap_pagedir(md->pagedir, (addr_t)addr, num_pages);
	return 0;
}

void
md_thread_set_entrypoint(thread_t thread, addr_t entry)
{
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;
	md->ctx.eip = entry;
}

/* vim:set ts=2 sw=2: */
