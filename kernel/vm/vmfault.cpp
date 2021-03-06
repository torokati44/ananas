#include <ananas/types.h>
#include <machine/param.h> /* for PAGE_SIZE */
#include <machine/vm.h> /* for md_{,un}map_pages() */
#include <ananas/error.h>
#include <ananas/lib.h>
#include <ananas/process.h>
#include <ananas/trace.h>
#include <ananas/kmem.h>
#include <ananas/pcpu.h>
#include <ananas/vm.h>
#include <ananas/lib.h>
#include <ananas/vmpage.h>
#include <ananas/vfs/core.h>
#include <ananas/vmspace.h>

TRACE_SETUP;

namespace {

errorcode_t
read_data(struct DENTRY* dentry, void* buf, off_t offset, size_t len)
{
	struct VFS_FILE f;
	memset(&f, 0, sizeof(f));
	f.f_dentry = dentry;

	errorcode_t err = vfs_seek(&f, offset);
	ANANAS_ERROR_RETURN(err);

	size_t amount = len;
	err = vfs_read(&f, buf, &amount);
	ANANAS_ERROR_RETURN(err);

	if (amount != len)
		return ANANAS_ERROR(SHORT_READ);
	return ananas_success();
}

int
vmspace_page_flags_from_va(vmarea_t* va)
{
	int flags = 0;
	if ((va->va_flags & (VM_FLAG_READ | VM_FLAG_WRITE)) == VM_FLAG_READ) {
		flags |= VM_PAGE_FLAG_READONLY;
	}
	return flags;
}

} // unnamed namespace

errorcode_t
vmspace_handle_fault(vmspace_t* vs, addr_t virt, int flags)
{
	TRACE(VM, INFO, "vmspace_handle_fault(): vs=%p, virt=%p, flags=0x%x", vs, virt, flags);

	/* Walk through the areas one by one */
	LIST_FOREACH(&vs->vs_areas, va, vmarea_t) {
		if (!(virt >= va->va_virt && (virt < (va->va_virt + va->va_len))))
			continue;

		/* We should only get faults for lazy areas (filled by a function) or when we have to dynamically allocate things */
		KASSERT((va->va_flags & (VM_FLAG_ALLOC | VM_FLAG_LAZY)) != 0, "unexpected pagefault in area %p, virt=%p, len=%d, flags 0x%x", va, va->va_virt, va->va_len, va->va_flags);

		// XXX lookup the page here, perhaps we are going from COW a new copy
		//struct VM_PAGE* vp =

		// XXX we expect va_doffset to be page-aligned here (i.e. we can always use a page directly)
		// this needs to be enforced when making mappings!

		// If there is a dentry attached here, perhaps we may find what we need in the corresponding inode
		if (va->va_dentry != nullptr) {
			addr_t v_page = virt & ~(PAGE_SIZE - 1);
			off_t read_off = v_page - va->va_virt; // offset in area, still needs va_doffset added
			if (read_off < va->va_dlength) {
				// At least (part of) the page is to be read from disk - this means we want
				// the entire page
				read_off += va->va_doffset;
				struct VM_PAGE* vmpage = vmpage_lookup_locked(va, va->va_dentry->d_inode, read_off);
				if (vmpage == nullptr) {
					// Page not found - we need to allocate one. This is always a shared mapping, which we'll copy if needed
					vmpage = vmpage_create_shared(va, va->va_dentry->d_inode, read_off, VM_PAGE_FLAG_PENDING | vmspace_page_flags_from_va(va));
				}
				// vmpage will be locked at this point!

				if (vmpage->vp_flags & VM_PAGE_FLAG_PENDING) {
					// Read the page - note that we hold the vmpage lock while doing this
					struct PAGE* p;
					void* page = page_alloc_single_mapped(&p, VM_FLAG_READ | VM_FLAG_WRITE);
					KASSERT(p != nullptr, "out of memory"); // XXX handle this

					errorcode_t err = read_data(va->va_dentry, page, read_off, PAGE_SIZE);
					kmem_unmap(page, PAGE_SIZE);
					KASSERT(ananas_is_success(err), "cannot deal with error %d", err); // XXX

					// Update the vm page to contain our new address
					vmpage->vp_page = p;
					vmpage->vp_flags &= ~VM_PAGE_FLAG_PENDING;
				}

				// If the mapping is page-aligned and read-only or shared, we can re-use the
				// mapping and avoid the entire copy
				struct VM_PAGE* new_vp;
				bool is_whole_page = (read_off + PAGE_SIZE) <= (va->va_doffset + va->va_dlength);
				//is_whole_page = false; // xxx
				if (is_whole_page && (va->va_flags & VM_FLAG_PRIVATE) == 0) {
					new_vp = vmpage_link(vmpage);
				} else {
					//kprintf("could NOT share %p (%x, %u, %u)\n", v_page, va->va_flags, (int)(read_off + PAGE_SIZE), (int)(va->va_doffset + va->va_dlength));
					// Cannot re-use; create a new VM page, with appropriate flags based on the va
					new_vp = vmpage_create_private(VM_PAGE_FLAG_PRIVATE | vmspace_page_flags_from_va(va));

					// Copy the content over - XXX handle zeroing out of unused parts
					vmpage_copy(vmpage, new_vp);
				}
				vmpage_unlock(vmpage);

				LIST_APPEND(&va->va_pages, new_vp);
				new_vp->vp_vaddr = virt & ~(PAGE_SIZE - 1);

				// Finally, update the permissions and we are done
				struct PAGE* new_p = vmpage_get_page(new_vp);
				md_map_pages(vs, new_vp->vp_vaddr, page_get_paddr(new_p), 1, va->va_flags);
				return ananas_success();
			}
		}

		// We need a new VM page here; this is an anonymous mapping which we need to back
		struct VM_PAGE* new_vp = vmpage_create_private(VM_PAGE_FLAG_PRIVATE);
		LIST_APPEND(&va->va_pages, new_vp);
		struct PAGE* new_p = vmpage_get_page(new_vp);
		new_vp->vp_vaddr = virt & ~(PAGE_SIZE - 1);

		// Clear the page XXX This is unfortunate, we should have a supply of pre-zeroed pages
		md_map_pages(vs, new_vp->vp_vaddr, page_get_paddr(new_p), 1, VM_FLAG_READ | VM_FLAG_WRITE);
		memset((void*)new_vp->vp_vaddr, 0, PAGE_SIZE);

		// And now (re)map the page for the caller
		md_map_pages(vs, new_vp->vp_vaddr, page_get_paddr(new_p), 1, va->va_flags);
		return ananas_success();
	}

	return ANANAS_ERROR(BAD_ADDRESS);
}

/* vim:set ts=2 sw=2: */
