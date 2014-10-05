#include <common/errno.h>
#include <syscall/mm.h>
#include <syscall/vfs.h>
#include <log.h>

#include <stdint.h>
#include <Windows.h>
#include <Psapi.h>
#include <ntdll.h>

/* Linux mmap() allows mapping into 4kB page boundaries, while Windows only
 * allows 64kB boundaries (called allocation granularity), although both
 * systems use 4kB page size.
 *
 * This difference causes two main issues for mmap() implementation:
 * 1. Map non 64kB aligned starting address of a file
 *     It's impossible to use Windows file mapping functions. We have to
 *     read/write file content manually on mmap()/msync()/munmap() calls.
 *     This may be slow. But we can possibly implement demand paging to
 *     improve performance.
 *
 * 2. Use MAP_FIXED with non 64kB aligned address
 *     We can allocate full 64kB aligned memory blocks and do partial
 *     allocations inside them. Note it seems impossible to implement
 *     MAP_FIXED with MAP_SHARED or MAP_PRIVATE on non 64kB aligned address.
 */

/* Overall memory layout
 *
 * FFFFFFFF ------------------------------
 * ...        Win32 kernel address space
 * ...         (unused if 4gt enabled)
 * 80000000 ------------------------------
 * ...                win32 dlls
 * 72000000 ------------------------------
 * ...        Foreign Linux kernel data
 * 70000000 ------------------------------
 * ...
 * ...          Application code/data
 * ...
 * 04000000 ------------------------------
 * ...            Win32 system heaps
 * ...        Foreign Linux kernel code
 * 00000000 ------------------------------
 *
 *
 * Foreign Linux kernel data memory layout
 *
 * 72000000 ------------------------------
 *                    kernel heap
 * 71000000 ------------------------------
 *                fork_info structure
 * 70FF0000 ------------------------------
 *             startup (argv, env) data
 * 70FE0000 ------------------------------
 *                tls_data structure
 * 70FD0000 ------------------------------
 *                vfs_data structure
 * 70900000 ------------------------------
 *              mm_heap_data structure
 * 70800000 ------------------------------
 *        process_data structure(unmappable)
 * 70700000 ------------------------------
 *           mm_data structure(unmappable)
 * 70000000 ------------------------------
 */

/* Hard limits */
/* Maximum number of mmap()-ed areas */
#define MAX_MMAP_COUNT 65535

/* OS-specific constants */
/* Lower bound of the virtual address space */
#define ADDRESS_SPACE_LOW 0x00000000U
/* Higher bound of the virtual address space */
#define ADDRESS_SPACE_HIGH 0x80000000U
/* The lowest non fixed allocation address we can make */
#define ADDRESS_ALLOCATION_LOW 0x04000000U
/* The highest non fixed allocation address we can make */
#define ADDRESS_ALLOCATION_HIGH 0x70000000U

#define BLOCK_COUNT 0x00010000U
#define PAGE_COUNT 0x00100000U
#define PAGES_PER_BLOCK 16

/* Helper macros */
#define IS_ALIGNED(addr, alignment) ((size_t) (addr) % (size_t) (alignment) == 0)
#define ALIGN_TO_BLOCK(addr) (((size_t) addr + BLOCK_SIZE - 1) & 0xFFFF0000)
#define ALIGN_TO_PAGE(addr) (((size_t) addr + PAGE_SIZE - 1) & 0xFFFFF000)
#define GET_BLOCK(addr) ((size_t) (addr) / BLOCK_SIZE)
#define GET_PAGE(addr) ((size_t) (addr) / PAGE_SIZE)
#define GET_PAGE_IN_BLOCK(page) ((page) % PAGES_PER_BLOCK)
#define GET_BLOCK_OF_PAGE(page) ((page) / PAGES_PER_BLOCK)
#define GET_FIRST_PAGE_OF_BLOCK(block)	((block) * PAGES_PER_BLOCK)
#define GET_BLOCK_ADDRESS(block) (void *)((block) * BLOCK_SIZE)
#define GET_PAGE_ADDRESS(page) (void *)((page) * PAGE_SIZE)

struct map_entry
{
	uint32_t start_page;
	uint32_t end_page;
	struct file *f;
	off_t offset_pages;
	struct map_entry *next;
};

struct mm_data
{
	/* Program break address, brk() will use this */
	void *brk;

	/* Information for all existing mappings */
	struct map_entry *map_list, *map_free_list;
	struct map_entry map_entries[MAX_MMAP_COUNT];

	/* Section object handle of a block */
	HANDLE block_section_handle[BLOCK_COUNT];
	/* Number of allocated pages inside a allocation region */
	uint8_t block_page_count[BLOCK_COUNT];

	/* Protection flags for a given page */
	uint8_t page_prot[PAGE_COUNT];
};
static struct mm_data *const mm = MM_DATA_BASE;

static struct map_entry *new_map_entry()
{
	if (mm->map_free_list)
	{
		struct map_entry *entry = mm->map_free_list;
		mm->map_free_list = mm->map_free_list->next;
		return entry;
	}
	return NULL;
}

static void free_map_entry(struct map_entry *entry)
{
	entry->next = mm->map_free_list;
	mm->map_free_list = entry;
}

void mm_init()
{
	VirtualAlloc(MM_DATA_BASE, sizeof(struct mm_data), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	/* Initialize mapping info freelist */
	for (uint32_t i = 0; i + 1 < MAX_MMAP_COUNT; i++)
		mm->map_entries[i].next = &mm->map_entries[i + 1];
	mm->map_list = NULL;
	mm->map_free_list = &mm->map_entries[0];
	mm->map_entries[MAX_MMAP_COUNT - 1].next = NULL;
}

void mm_reset()
{
	/* Release all user memory */
	for (uint32_t i = GET_BLOCK(ADDRESS_ALLOCATION_LOW); i < GET_BLOCK(ADDRESS_ALLOCATION_HIGH); i++)
		if (mm->block_section_handle[i])
		{
			NtUnmapViewOfSection(NtCurrentProcess(), GET_BLOCK_ADDRESS(i));
			NtClose(mm->block_section_handle[i]);
			mm->block_section_handle[i] = NULL;
			mm->block_page_count[i] = 0;
		}
	for (struct map_entry *e = mm->map_list, *p = NULL; e;)
	{
		if (e->start_page >= GET_PAGE(ADDRESS_ALLOCATION_LOW) && e->end_page < GET_PAGE(ADDRESS_ALLOCATION_HIGH))
		{
			for (uint32_t i = e->start_page; i <= e->end_page; i++)
				mm->page_prot[i] = 0;
			if (p)
				p->next = e->next;
			else
				mm->map_list = e->next;
			struct map_entry *t = e;
			e = e->next;
			free_map_entry(t);
		}
		else
			p = e, e = e->next;
	}
}

void mm_shutdown()
{
	for (uint32_t i = 0; i < BLOCK_COUNT; i++)
		if (mm->block_section_handle[i])
		{
			NtUnmapViewOfSection(NtCurrentProcess(), GET_BLOCK_ADDRESS(i));
			NtClose(mm->block_section_handle[i]);
		}
	VirtualFree(mm, 0, MEM_RELEASE);
}

void mm_update_brk(void *brk)
{
	mm->brk = max(mm->brk, brk);
}

/* Find 'count' consecutive free pages in address range [low, high), return 0 if not found */
static uint32_t find_free_pages(uint32_t count, uint32_t low, uint32_t high)
{
	uint32_t last = GET_PAGE(low);
	for (struct map_entry *e = mm->map_list; e; e = e->next)
		if (e->start_page >= GET_PAGE(low))
			if (e->start_page - last >= count)
				return last;
			else
				last = e->end_page + 1;
	if (GET_PAGE(high) - last >= count)
		return last;
	else
		return 0;
}

uint32_t mm_find_free_pages(uint32_t count_bytes)
{
	return find_free_pages(GET_PAGE(ALIGN_TO_PAGE(count_bytes)), ADDRESS_ALLOCATION_LOW, ADDRESS_ALLOCATION_HIGH);
}

static DWORD prot_linux2win(int prot)
{
	if ((prot & PROT_EXEC) && (prot & PROT_WRITE))
		return PAGE_EXECUTE_READWRITE;
	else if ((prot & PROT_EXEC) && (prot & PROT_READ))
		return PAGE_EXECUTE_READ;
	else if ((prot & PROT_EXEC))
		return PAGE_EXECUTE; /* Unsupported for file mapping */
	else if (prot & PROT_WRITE)
		return PAGE_READWRITE;
	else if (prot & PROT_READ)
		return PAGE_READONLY;
	else
		return PAGE_NOACCESS;
}

static void dump_virtual_memory(HANDLE process)
{
	char *addr = 0;
	do
	{
		MEMORY_BASIC_INFORMATION info;
		VirtualQueryEx(process, addr, &info, sizeof(info));
		if (info.State != MEM_FREE)
		{
			char filename[1024];
			if (GetMappedFileNameA(process, addr, filename, sizeof(filename)))
				log_debug("0x%08x - 0x%08x <--- %s\n", info.BaseAddress, (uint32_t)info.BaseAddress + info.RegionSize, filename);
			else
				log_debug("0x%08x - 0x%08x\n", info.BaseAddress, (uint32_t)info.BaseAddress + info.RegionSize);
		}
		addr += info.RegionSize;
	} while ((uint32_t)addr < 0x7FFF0000);
}

void mm_dump_stack_trace(PCONTEXT context)
{
	log_debug("Stack trace:\n");
	uint32_t esp = context->Esp;
	for (uint32_t i = (esp - 256) & ~15; i < esp; i += 16)
	{
		log_debug("%08x ", i);
		for (uint32_t j = i; j < i + 16 && j < esp; j++)
			log_debug("%02x ", *(unsigned char *)j);
		log_debug("\n");
	}
}

static HANDLE duplicate_section(HANDLE source, void *source_addr)
{
	HANDLE dest;
	PVOID dest_addr = NULL;
	OBJECT_ATTRIBUTES attr;
	attr.Length = sizeof(OBJECT_ATTRIBUTES);
	attr.RootDirectory = NULL;
	attr.ObjectName = NULL;
	attr.Attributes = OBJ_INHERIT;
	attr.SecurityDescriptor = NULL;
	attr.SecurityQualityOfService = NULL;
	LARGE_INTEGER max_size;
	max_size.QuadPart = BLOCK_SIZE;
	SIZE_T view_size = BLOCK_SIZE;
	NTSTATUS status;

	status = NtCreateSection(&dest, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE, &attr, &max_size, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
	if (status != STATUS_SUCCESS)
		return NULL;
	
	NtMapViewOfSection(dest, NtCurrentProcess(), &dest_addr, 0, BLOCK_SIZE, NULL, &view_size, ViewUnmap, 0, PAGE_READWRITE);
	/* Mark source block entirely readable. TODO: Find a better way */
	DWORD oldProtect;
	VirtualProtect(source_addr, BLOCK_SIZE, PAGE_EXECUTE_READ, &oldProtect);
	CopyMemory(dest_addr, source_addr, BLOCK_SIZE);
	NtUnmapViewOfSection(NtCurrentProcess(), dest_addr);
	return dest;
}

int mm_handle_page_fault(void *addr)
{
	log_debug("Handling page fault at address %x (page %x)\n", addr, GET_PAGE(addr));
	if ((size_t)addr < ADDRESS_SPACE_LOW || (size_t)addr >= ADDRESS_SPACE_HIGH)
	{
		log_debug("Address %x outside of valid usermode address space.\n", addr);
		return 0;
	}
	if ((mm->page_prot[GET_PAGE(addr)] & PROT_WRITE) == 0)
	{
		log_debug("Address %x (page %x) not writable.\n", addr, GET_PAGE(addr));
		return 0;
	}
	uint16_t block = GET_BLOCK(addr);
	if (mm->block_section_handle[block] == NULL)
	{
		log_debug("Address %x (page %x) not mapped.\n", addr, GET_PAGE(addr));
		return 0;
	}
	/* Query information about the section object which the page within */
	OBJECT_BASIC_INFORMATION info;
	NTSTATUS status;
	status = NtQueryObject(mm->block_section_handle[block], ObjectBasicInformation, &info, sizeof(OBJECT_BASIC_INFORMATION), NULL);
	if (status != STATUS_SUCCESS)
	{
		log_debug("NtQueryObject() on section %x failed.\n", block);
		return 0;
	}
	if (info.HandleCount == 1)
		log_debug("We're the only owner, simply change protection flags.\n");
	else
	{
		/* We are not the only one holding the section, duplicate it */
		log_debug("Duplicating section %x...\n", block);
		HANDLE section;
		if (!(section = duplicate_section(mm->block_section_handle[block], GET_BLOCK_ADDRESS(block))))
		{
			log_debug("Duplicating section failed.\n");
			return 0;
		}
		else
			log_debug("Duplicating section succeeded. Remapping...\n");
		status = NtUnmapViewOfSection(NtCurrentProcess(), GET_BLOCK_ADDRESS(block));
		if (status != STATUS_SUCCESS)
		{
			log_debug("Unmapping failed, status: %x\n", status);
			return 0;
		}
		NtClose(mm->block_section_handle[block]);
		mm->block_section_handle[block] = section;
		PVOID base_addr = GET_BLOCK_ADDRESS(block);
		SIZE_T view_size = BLOCK_SIZE;
		status = NtMapViewOfSection(section, NtCurrentProcess(), &base_addr, 0, BLOCK_SIZE, NULL, &view_size, ViewUnmap, 0, PAGE_EXECUTE_READWRITE);
		if (status != STATUS_SUCCESS)
		{
			log_debug("Remapping failed, status: %x\n", status);
			return 0;
		}
	}
	/* We're the only owner of the section now, change page protection flags */
	for (uint32_t i = 0; i < PAGES_PER_BLOCK; i++)
	{
		uint32_t page = GET_FIRST_PAGE_OF_BLOCK(block) + i;
		DWORD oldProtect;
		if (!VirtualProtect(GET_PAGE_ADDRESS(page), PAGE_SIZE, prot_linux2win(mm->page_prot[page]), &oldProtect))
		{
			log_debug("VirtualProtect(0x%x) failed, error code: %d.\n", GET_PAGE_ADDRESS(page), GetLastError());
			return 0;
		}
	}
	return 1;
}

int mm_fork(HANDLE process)
{
	/* Copy mm_data struct */
	if (!VirtualAllocEx(process, MM_DATA_BASE, sizeof(struct mm_data), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
	{
		log_debug("mm_fork(): Allocate mm_data structure failed, error code: %d\n", GetLastError());
		return 0;
	}
	if (!WriteProcessMemory(process, MM_DATA_BASE, mm, sizeof(struct mm_data), NULL))
	{
		log_debug("mm_fork(): Write mm_data structure failed, error code: %d\n", GetLastError());
		return 0;
	}
	/* Map sections */
	for (uint32_t i = 0; i < BLOCK_COUNT; i++)
		if (mm->block_section_handle[i])
		{
			PVOID base_addr = GET_BLOCK_ADDRESS(i);
			SIZE_T view_size = BLOCK_SIZE;
			NTSTATUS status;
			status = NtMapViewOfSection(mm->block_section_handle[i], process, &base_addr, 0, BLOCK_SIZE, NULL, &view_size, ViewUnmap, 0, PAGE_EXECUTE_READWRITE);
			if (status != STATUS_SUCCESS)
			{
				log_debug("mm_fork(): Map failed: %x, status code: %x\n", base_addr, status);
				dump_virtual_memory(process);
				return 0;
			}
		}
	/* Disable write permission on pages */
	for (struct map_entry *e = mm->map_list; e; e = e->next)
		for (uint32_t j = e->start_page; j <= e->end_page; j++)
		{
			DWORD oldProtect;
			if (!VirtualProtectEx(process, GET_PAGE_ADDRESS(j), PAGE_SIZE, prot_linux2win(mm->page_prot[j] & ~PROT_WRITE), &oldProtect))
			{
				log_debug("VirtualProtectEx(%x) on child failed.\n", GET_PAGE_ADDRESS(j));
				return 0;
			}
			if (!VirtualProtect(GET_PAGE_ADDRESS(j), PAGE_SIZE, prot_linux2win(mm->page_prot[j] & ~PROT_WRITE), &oldProtect))
			{
				log_debug("VirtualProtect(%x) failed.\n", GET_PAGE_ADDRESS(j));
				return 0;
			}
		}
	return 1;
}

void *mm_mmap(void *addr, size_t length, int prot, int flags, struct file *f, off_t offset_pages)
{
	if (length == 0)
		return -EINVAL;
	length = ALIGN_TO_PAGE(length);
	if ((size_t)addr < ADDRESS_SPACE_LOW || (size_t)addr >= ADDRESS_SPACE_HIGH
		|| (size_t)addr + length < ADDRESS_SPACE_LOW || (size_t)addr + length >= ADDRESS_SPACE_HIGH
		|| (size_t)addr + length < (size_t)addr)
		return -EINVAL;
	if (flags & MAP_SHARED)
	{
		log_debug("MAP_SHARED is not supported yet.\n");
		return -EINVAL;
	}
	if ((flags & MAP_ANONYMOUS) && f != NULL)
	{
		log_debug("MAP_ANONYMOUS with file descriptor.\n");
		return -EINVAL;
	}
	if (!(flags & MAP_ANONYMOUS) && f == NULL)
	{
		log_debug("MAP_FILE with bad file descriptor.\n");
		return -EBADF;
	}
	if (!(flags & MAP_FIXED))
	{
		uint32_t alloc_page;
		if (flags & __MAP_HEAP)
			alloc_page = find_free_pages(GET_PAGE(ALIGN_TO_PAGE(length)), HEAP_BASE, ADDRESS_ALLOCATION_LOW);
		else
			alloc_page = find_free_pages(GET_PAGE(ALIGN_TO_PAGE(length)), ADDRESS_ALLOCATION_LOW, ADDRESS_ALLOCATION_HIGH);
		if (!alloc_page)
		{
			log_debug("Cannot find free pages.\n");
			return -ENOMEM;
		}

		addr = GET_PAGE_ADDRESS(alloc_page);
	}
	if ((flags & MAP_FIXED) && !IS_ALIGNED(addr, PAGE_SIZE))
	{
		log_debug("Not aligned addr with MAP_FIXED.\n");
		return -EINVAL;
	}

	uint32_t start_page = GET_PAGE(addr);
	uint32_t end_page = GET_PAGE((size_t)addr + length - 1);
	uint16_t start_block = GET_BLOCK(addr);
	uint16_t end_block = GET_BLOCK((size_t)addr + length - 1);

	/*
	If address are fixed, unmap conflicting pages,
	Otherwise the pages are found by find_free_pages() thus are guaranteed free.
	*/
	if ((flags & MAP_FIXED))
		mm_munmap(addr, length);

	if (!(flags & MAP_ANONYMOUS))
		prot |= PROT_WRITE; /* TODO: Fix at end */

	/* Allocate and map missing section objects */
	for (uint16_t i = start_block; i <= end_block; i++)
	{
		if (mm->block_page_count[i] == 0)
		{
			OBJECT_ATTRIBUTES attr;
			attr.Length = sizeof(OBJECT_ATTRIBUTES);
			attr.RootDirectory = NULL;
			attr.ObjectName = NULL;
			attr.Attributes = OBJ_INHERIT;
			attr.SecurityDescriptor = NULL;
			attr.SecurityQualityOfService = NULL;
			LARGE_INTEGER max_size;
			max_size.QuadPart = BLOCK_SIZE;
			NTSTATUS status;
			HANDLE handle;

			/* Allocate section */
			status = NtCreateSection(&handle, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE, &attr, &max_size, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);
			if (status != STATUS_SUCCESS)
			{
				log_debug("NtCreateSection() failed. Status: %x\n", status);
				goto ROLLBACK;
			}

			/* Map section */
			PVOID base_addr = GET_BLOCK_ADDRESS(i);
			SIZE_T view_size = BLOCK_SIZE;
			status = NtMapViewOfSection(handle, NtCurrentProcess(), &base_addr, 0, BLOCK_SIZE, NULL, &view_size, ViewUnmap, 0, PAGE_EXECUTE_READWRITE);
			if (status != STATUS_SUCCESS)
			{
				log_debug("NtMapViewOfSection() failed. Address: %x, Status: %x\n", base_addr, status);
				NtClose(handle);
				dump_virtual_memory(NtCurrentProcess());
				goto ROLLBACK;
			}
			mm->block_section_handle[i] = handle;
			continue;

		ROLLBACK:
			/* Roll back */
			for (uint16_t j = start_block; j < i; j++)
			{
				if (mm->block_page_count[j] == 0)
				{
					NtUnmapViewOfSection(NtCurrentProcess(), GET_BLOCK_ADDRESS(j));
					NtClose(mm->block_section_handle[j]);
					mm->block_section_handle[j] = NULL;
				}
			}
			return -ENOMEM; /* TODO */
		}
	}

	/* Set up all kinds of flags */
	struct map_entry *entry = new_map_entry();
	entry->start_page = start_page;
	entry->end_page = end_page;
	entry->f = f;
	entry->offset_pages = offset_pages;
	if (f) /* TODO: Implement on demand paging? */
	{
		f->ref++;
		f->op_vtable->pread(f, (char*)(start_page * PAGE_SIZE), (end_page - start_page + 1) * PAGE_SIZE, (loff_t)offset_pages * PAGE_SIZE);
	}
	if (!mm->map_list || mm->map_list->start_page > end_page)
	{
		if (mm->map_list)
			entry->next = mm->map_list->next;
		mm->map_list = entry;
	}
	else
	{
		for (struct map_entry *e = mm->map_list; e; e = e->next)
			if (!e->next || e->next->start_page > end_page)
			{
				entry->next = e->next;
				e->next = entry;
				break;
			}
	}
	for (uint32_t i = start_page; i <= end_page; i++)
	{
		/* TODO: Optimization */
		mm->page_prot[i] = prot;
		mm->block_page_count[GET_BLOCK_OF_PAGE(i)]++;
		DWORD old;
		if (!VirtualProtect(GET_PAGE_ADDRESS(i), PAGE_SIZE, prot_linux2win(prot), &old))
		{
			log_debug("VirtualProtect() failed, error code: %d\n", GetLastError());
			return -ENOMEM; /* TODO */
		}
	}
	log_debug("Allocated memory: %x\n", addr);
	return addr;
}

int mm_munmap(void *addr, size_t length)
{
	/* TODO: We should mark NOACCESS for munmap()-ed but not VirtualFree()-ed pages */
	/* TODO: We currently only support unmap full pages */
	if (!IS_ALIGNED(addr, PAGE_SIZE))
		return -EINVAL;
	length = ALIGN_TO_PAGE(length);
	if ((size_t)addr < ADDRESS_SPACE_LOW || (size_t)addr >= ADDRESS_SPACE_HIGH
		|| (size_t)addr + length < ADDRESS_SPACE_LOW || (size_t)addr + length >= ADDRESS_SPACE_HIGH
		|| (size_t)addr + length < (size_t)addr)
	{
		return -EINVAL;
	}

	uint32_t unmap_start_page = GET_PAGE(addr);
	uint32_t unmap_end_page = GET_PAGE((size_t)addr + length - 1);
	struct map_entry *pred = NULL;
	for (struct map_entry *e = mm->map_list; e;)
		if (e->start_page > unmap_end_page)
			break;
		else if (e->end_page >= unmap_start_page) /* Conflict found */
		{
			/* Determined overlapped pages */
			uint32_t start_page = max(unmap_start_page, e->start_page);
			uint32_t end_page = min(unmap_end_page, e->end_page);
			/* Modify entry */
			if (start_page > e->start_page && end_page < e->end_page)
			{
				/* Need to split entry */
				struct map_entry *ne = new_map_entry();
				ne->start_page = end_page + 1;
				ne->end_page = e->end_page;
				if (ne->f = e->f)
					ne->offset_pages = e->offset_pages + (ne->start_page - e->start_page);
				e->end_page = start_page - 1;
				ne->next = e->next;
				e->next = ne;
				pred = e;
				e = e->next;
			}
			else if (start_page > e->start_page)
			{
				e->end_page = start_page - 1;
				pred = e;
				e = e->next;
			}
			else if (end_page < e->end_page)
			{
				if (e->f)
					e->offset_pages += end_page + 1 - e->start_page;
				e->start_page = end_page + 1;
				pred = e;
				e = e->next;
			}
			else
			{
				/* Remove entry from entry list */
				if (e->f) /* Release file handle if used */
					vfs_release(e->f);
				if (pred == NULL)
					mm->map_list = e->next;
				else
					pred->next = e->next;
				struct map_entry *tmp = e;
				e = e->next;
				/* Add entry to free list */
				free_map_entry(tmp);
			}
			for (uint32_t i = start_page; i <= end_page; i++)
				mm->block_page_count[GET_BLOCK_OF_PAGE(i)]--; /* TODO: Optimization */
			/* Free unused memory allocations */
			uint16_t start_block = GET_BLOCK_OF_PAGE(start_page);
			uint16_t end_block = GET_BLOCK_OF_PAGE(end_page);
			for (uint16_t i = start_block; i <= end_block; i++)
			{
				if (mm->block_page_count[i] == 0)
				{
					NtUnmapViewOfSection(NtCurrentProcess(), GET_BLOCK_ADDRESS(i));
					NtClose(mm->block_section_handle[i]);
					mm->block_section_handle[i] = NULL;
				}
			}
		}
		else
		{
			pred = e;
			e = e->next;
		}
	return 0;
}

void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	/* TODO: We should mark NOACCESS for VirtualAlloc()-ed but currently unused pages */
	log_debug("mmap(%x, %x, %x, %x, %d, %x)\n", addr, length, prot, flags, fd, offset);
	/* TODO: Initialize mapped area to zero */
	if (!IS_ALIGNED(offset, PAGE_SIZE))
		return -EINVAL;
	return mm_mmap(addr, length, prot, flags, vfs_get(fd), offset / PAGE_SIZE);
}

void *sys_oldmmap(void *_args)
{
	log_debug("oldmmap(%x)\n", _args);
	struct oldmmap_args_t
	{
		void *addr;
		unsigned long len;
		unsigned long prot;
		unsigned long flags;
		unsigned long fd;
		unsigned long offset;
	};
	struct oldmmap_args_t *args = _args;
	return sys_mmap(args->addr, args->len, args->prot, args->flags, args->fd, args->offset);
}

void *sys_mmap2(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	log_debug("mmap2(%x, %x, %x, %x, %d, %x)\n", addr, length, prot, flags, fd, offset);
	return mm_mmap(addr, length, prot, flags, vfs_get(fd), offset);
}

int sys_munmap(void *addr, size_t length)
{
	log_debug("munmap(%x, %x)\n", addr, length);
	return mm_munmap(addr, length);
}

int sys_mprotect(void *addr, size_t length, int prot)
{
	log_debug("mprotect(%x, %x, %x)\n", addr, length, prot);
	if (!IS_ALIGNED(addr, PAGE_SIZE))
		return -EINVAL;
	length = ALIGN_TO_PAGE(length);
	if ((size_t)addr < ADDRESS_SPACE_LOW || (size_t)addr >= ADDRESS_SPACE_HIGH
		|| (size_t)addr + length < ADDRESS_SPACE_LOW || (size_t)addr + length >= ADDRESS_SPACE_HIGH
		|| (size_t)addr + length < (size_t)addr)
	{
		return -EINVAL;
	}
	/* Validate all pages are mapped */
	uint32_t start_page = GET_PAGE(addr);
	uint32_t end_page = GET_PAGE((uint32_t)addr + length - 1);
	uint32_t last_page = start_page - 1;
	for (struct map_entry *e = mm->map_list; e; e = e->next)
		if (e->start_page > end_page)
			break;
		else if (e->end_page >= start_page)
		{
			if (e->start_page == last_page + 1)
				last_page = e->end_page;
			else
				break;
		}
	if (last_page < end_page)
		return -ENOMEM;
	;
	/* Change protection flags */
	uint32_t j = start_page;
	for (uint32_t i = start_page; i <= end_page + 1; i++)
		if (mm->page_prot[i] != mm->page_prot[j] || i == end_page + 1)
		{
			int old_prot = mm->page_prot[j];
			DWORD protection, oldProtection;
			if (old_prot & PROT_WRITE)
				protection = prot_linux2win(prot);
			else
				protection = prot_linux2win(prot & ~PROT_WRITE);
			/* Change protection flags for pages in [j, i) */
			uint32_t start_block = GET_BLOCK_OF_PAGE(j);
			uint32_t end_block = GET_BLOCK_OF_PAGE(i - 1);
			for (uint32_t k = start_block; k < end_block; k++)
			{
				VirtualProtect(GET_PAGE_ADDRESS(j), (PAGES_PER_BLOCK - GET_PAGE_IN_BLOCK(j)) * PAGE_SIZE, protection, &oldProtection);
				j = (k + 1) * PAGES_PER_BLOCK;
			}
			if (i > j)
				VirtualProtect(GET_PAGE_ADDRESS(j), (i - j) * PAGE_SIZE, protection, &oldProtection);
		}
	for (uint32_t i = start_page; i <= end_page; i++)
		mm->page_prot[i] = prot;
	return 0;
}

int sys_msync(void *addr, size_t len, int flags)
{
	/* TODO */
}

int sys_mlock(const void *addr, size_t len)
{
	/* TODO */
}

int sys_munlock(const void *addr, size_t len)
{
	/* TODO */
}

void *sys_brk(void *addr)
{
	log_debug("brk(%x)\n", addr);
	log_debug("Last brk: %x\n", mm->brk);
	uint32_t brk = ALIGN_TO_PAGE(mm->brk);
	addr = ALIGN_TO_PAGE(addr);
	/* TODO: Handle brk shrink */
	if (addr > mm->brk)
	{
		if (sys_mmap(brk, (uint32_t)addr - (uint32_t)brk, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0) < 0)
		{
			log_debug("Enlarge brk failed.\n");
			return -ENOMEM;
		}
		mm->brk = addr;
	}
	log_debug("New brk: %x\n", mm->brk);
	return mm->brk;
}
