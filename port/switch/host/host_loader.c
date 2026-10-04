/*
HOST_LOADER.C

Loads the guest image: a statically linked AArch64 ELF executable (built
from ILP32 code, see tools/guest_build.py) whose segments go to the
addresses it was linked at, below 4 GB, from 0x88000000. The image starts
with a struct halo_guest_header naming its import table, which is filled
with the host functions of the same names.

Code memory cannot be written once mapped (host_memory.c), so the image's
read-only segments (header, code, constants: they share pages) are put
together first and mapped at once, executable; the writable one is mapped
and then filled.
*/

#include "host.h"

#include <elf.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000ull

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("The game called a host function this version does not have.");
}

int host_load_image(const void *file, size_t size)
{
	const Elf64_Ehdr *elf = file;
	const Elf64_Phdr *segments;
	uint64_t low = ~0ull, high = 0, code_end = 0, data_start = ~0ull;
	const struct halo_guest_header *header;
	unsigned char *code;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	int missing = 0;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, ELFMAG, SELFMAG) || elf->e_ident[EI_CLASS] != ELFCLASS64 ||
		elf->e_machine != EM_AARCH64 || elf->e_type != ET_EXEC)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable");
		return -1;
	}
	segments = (const Elf64_Phdr *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf64_Phdr *segment = &segments[index];

		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_offset + segment->p_filesz > size)
			return -1;
		if (segment->p_vaddr < low)
			low = segment->p_vaddr;
		if (segment->p_vaddr + segment->p_memsz > high)
			high = segment->p_vaddr + segment->p_memsz;
		if (segment->p_flags & PF_W)
		{
			if (segment->p_vaddr < data_start)
				data_start = segment->p_vaddr;
		}
		else if (segment->p_vaddr + segment->p_memsz > code_end)
		{
			code_end = segment->p_vaddr + segment->p_memsz;
		}
	}
	low &= ~(PAGE - 1);
	high = (high + PAGE - 1) & ~(PAGE - 1);
	code_end = (code_end + PAGE - 1) & ~(PAGE - 1);
	if (low != HALO_GUEST_IMAGE_BASE || high > 0x100000000ull || data_start == ~0ull ||
		(data_start & (PAGE - 1)) || code_end > data_start)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's layout is unexpected (%llx-%llx, data at %llx)",
			(unsigned long long)low, (unsigned long long)high, (unsigned long long)data_start);
		return -1;
	}

	/* the read-only segments, then their mapping */
	code = calloc(1, code_end - low);
	if (!code)
		return -1;
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf64_Phdr *segment = &segments[index];

		if (segment->p_type == PT_LOAD && !(segment->p_flags & PF_W))
			memcpy(code + (segment->p_vaddr - low), (const char *)file + segment->p_offset, segment->p_filesz);
	}
	if (host_memory_map_code((uint32_t)low, code, (uint32_t)(code_end - low)) != 0)
	{
		free(code);
		return -1;
	}
	free(code);

	/* the writable segment (data and bss) */
	if (host_memory_map_data((uint32_t)data_start, (uint32_t)(high - data_start)) != 0)
		return -1;
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf64_Phdr *segment = &segments[index];

		if (segment->p_type == PT_LOAD && (segment->p_flags & PF_W))
			memcpy((void *)segment->p_vaddr, (const char *)file + segment->p_offset, segment->p_filesz);
	}

	header = (const struct halo_guest_header *)low;
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;

	/* the import table is in the data segment */
	table = (uint64_t *)(uintptr_t)header->import_table;
	name = (const char *)(uintptr_t)header->import_names;
	count = *(const uint32_t *)(uintptr_t)header->import_count;
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function && !strncmp(name, "hostgl_", 7))
			function = host_gl_resolve(name + 7);
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx (code to %08llx), %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, (unsigned long long)code_end, count, missing);
	return 0;
}
