#include <core/forward_list.h>
#include <dbt/x86.h>
#include <dbt/x86_inst.h>
#include <syscall/mm.h>
#include <syscall/tls.h>
#include <log.h>

#include <stdint.h>
#include <Windows.h>

#define GET_MODRM_MOD(c)	(((c) >> 6) & 7)
#define GET_MODRM_R(c)		(((c) >> 3) & 7)
#define GET_MODRM_RM(c)		((c) & 7)
#define GET_MODRM_CODE(c)	GET_MODRM_R(c)

#define GET_SIB_SCALE(s)	((s) >> 6)
#define GET_SIB_INDEX(s)	(((s) >> 3) & 7)
#define GET_SIB_BASE(s)		((s) & 7)

#define GET_REX_W(r)		(((r) >> 3) & 1)
#define GET_REX_R(r)		(((r) >> 2) & 1)
#define GET_REX_X(r)		(((r) >> 1) & 1)
#define GET_REX_B(r)		(r & 1)

/* ModR/M flags */
#define MODRM_PURE_REGISTER	1

struct modrm_rm_t
{
	int base, index, scale, flags;
	int32_t disp;
};

/* Helpers for constructing modrm_rm_t structure */
static struct modrm_rm_t __forceinline modrm_rm_reg(int r)
{
	struct modrm_rm_t rm;
	rm.base = r;
	rm.index = -1;
	rm.scale = 0;
	rm.disp = 0;
	rm.flags = MODRM_PURE_REGISTER;
	return rm;
}

static struct modrm_rm_t __forceinline modrm_rm_disp(int32_t disp)
{
	struct modrm_rm_t rm;
	rm.base = -1;
	rm.index = -1;
	rm.scale = 0;
	rm.disp = disp;
	rm.flags = 0;
	return rm;
}

static struct modrm_rm_t __forceinline modrm_rm_mreg(int base, int32_t disp)
{
	struct modrm_rm_t rm;
	rm.base = base;
	rm.index = -1;
	rm.scale = 0;
	rm.disp = disp;
	rm.flags = 0;
	return rm;
}

static struct modrm_rm_t __forceinline modrm_rm_mscale(int base, int index, int scale, int32_t disp)
{
	struct modrm_rm_t rm;
	rm.base = base;
	rm.index = index;
	rm.scale = scale;
	rm.disp = disp;
	rm.flags = 0;
	return rm;
}

static uint8_t __forceinline parse_byte(uint8_t **code)
{
	return *(*code)++;
}

static uint16_t __forceinline parse_word(uint8_t **code)
{
	return *((uint16_t*)*code)++;
}

static uint32_t __forceinline parse_dword(uint8_t **code)
{
	return *((uint32_t*)*code)++;
}

static uint64_t __forceinline parse_qword(uint8_t **code)
{
	return *((uint64_t*)*code)++;
}

static int32_t __forceinline parse_rel(uint8_t **code, int rel_bytes)
{
	if (rel_bytes == 1)
		return (int8_t)parse_byte(code);
	else if (rel_bytes == 2)
		return (int16_t)parse_word(code);
	else
		return (int32_t)parse_dword(code);
}

static void parse_modrm(uint8_t **code, int *r, struct modrm_rm_t *rm)
{
	uint8_t modrm = parse_byte(code);
	*r = GET_MODRM_R(modrm);
	int mod = GET_MODRM_MOD(modrm);
	if (mod == 3)
	{
		rm->flags = MODRM_PURE_REGISTER;
		rm->base = GET_MODRM_RM(modrm);
		rm->index = -1;
		return;
	}
	rm->flags = 0;
	int sib_bytes = 0;
	int modrm_rm = GET_MODRM_RM(modrm);
	if (modrm_rm == 4)
	{
		/* ModR/M with SIB byte */
		sib_bytes = 1;
		int sib = parse_byte(code);
		rm->scale = GET_SIB_SCALE(sib);
		if ((rm->index = GET_SIB_INDEX(sib)) == 4)
			rm->index = -1;
		if ((rm->base = GET_SIB_BASE(sib)) == 5 && mod == 0)
		{
			rm->base = -1;
			mod = 2; /* For use later to correctly extract disp32 */
		}
	}
	else
	{
		/* ModR/M without SIB byte */
		rm->index = -1;
		rm->scale = 0;
		if (mod == 0 && modrm_rm == 5) /* disp32 */
		{
			rm->base = -1;
			rm->disp = (int32_t)parse_dword(code);
			return;
		}
		rm->base = modrm_rm;
	}
	/* Displacement */
	if (mod == 1) /* disp8 */
		rm->disp = (int8_t)parse_byte(code);
	else if (mod == 2) /* disp32 */
		rm->disp = (int32_t)parse_dword(code);
	else /* no disp */
		rm->disp = 0;
}

static __forceinline void gen_byte(uint8_t **out, uint8_t x)
{
	*(*out)++ = x;
}

static __forceinline void gen_word(uint8_t **out, uint16_t x)
{
	*(uint16_t *)(*out) = x;
	*out += 2;
}

static __forceinline void gen_dword(uint8_t **out, uint32_t x)
{
	*(uint32_t *)(*out) = x;
	*out += 4;
}

static __forceinline void gen_qword(uint8_t **out, uint64_t x)
{
	*(uint64_t *)(*out) = x;
	*out += 8;
}

static __forceinline void gen_copy(uint8_t **out, uint8_t *code, int count)
{
	for (int i = 0; i < count; i++)
		gen_byte(out, *code++);
}

static __forceinline void gen_modrm(uint8_t **out, int mod, int r, int rm)
{
	gen_byte(out, (mod << 6) + (r << 3) + rm);
}

static __forceinline void gen_sib(uint8_t **out, int base, int index, int scale)
{
	gen_byte(out, (scale << 6) + (index << 3) + base);
}

static __forceinline void gen_modrm_sib(uint8_t **out, int r, struct modrm_rm_t rm)
{
	if (rm.flags == MODRM_PURE_REGISTER)
	{
		gen_modrm(out, 3, r, rm.base);
		return;
	}
	if (rm.index == 4)
	{
		log_error("gen_modrm(): rsp or r12 cannot be used as an index register.\n");
		return;
	}
	/* TODO: Use shorter codes when the offset is small */
	if (rm.base == -1 && rm.index == -1) /* disp32 */
	{
		gen_modrm(out, 0, r, 5);
		gen_dword(out, rm.disp);
	}
	else if (rm.base == -1) /* [scaled index] + disp32 */
	{
		gen_modrm(out, 0, r, 4);
		gen_sib(out, 5, rm.index, rm.scale);
		gen_dword(out, rm.disp);
	}
	else if (rm.base == 4 || rm.index != -1) /* SIB required */
	{
		gen_modrm(out, 2, r, 4);
		gen_sib(out, rm.base, rm.index == -1? 4: rm.index, rm.scale);
		gen_dword(out, rm.disp);
	}
	else
	{
		/* SIB not needed */
		gen_modrm(out, 2, r, rm.base);
		gen_dword(out, rm.disp);
	}
}

static __forceinline void gen_fs_prefix(uint8_t **out)
{
	gen_byte(out, 0x64);
}

static __forceinline void gen_mov_r_rm_16(uint8_t **out, int r, struct modrm_rm_t rm)
{
	gen_byte(out, 0x66);
	gen_byte(out, 0x8B);
	gen_modrm_sib(out, r, rm);
}

static __forceinline void gen_mov_rm_r_16(uint8_t **out, struct modrm_rm_t rm, int r)
{
	gen_byte(out, 0x66);
	gen_byte(out, 0x89);
	gen_modrm_sib(out, r, rm);
}

static __forceinline void gen_mov_r_rm_32(uint8_t **out, int r, struct modrm_rm_t rm)
{
	gen_byte(out, 0x8B);
	gen_modrm_sib(out, r, rm);
}

static __forceinline void gen_mov_rm_r_32(uint8_t **out, struct modrm_rm_t rm, int r)
{
	gen_byte(out, 0x89);
	gen_modrm_sib(out, r, rm);
}

static __forceinline void gen_shr_rm_32(uint8_t **out, struct modrm_rm_t rm, uint8_t imm8)
{
	gen_byte(out, 0xC1);
	gen_modrm_sib(out, 5, rm);
	gen_byte(out, imm8);
}

static __forceinline void gen_lea(uint8_t **out, int r, struct modrm_rm_t rm)
{
	gen_byte(out, 0x8D);
	gen_modrm_sib(out, r, rm);
}

static __forceinline void gen_popfd(uint8_t **out)
{
	gen_byte(out, 0x9D);
}

static __forceinline void gen_pop_rm(uint8_t **out, struct modrm_rm_t rm)
{
	gen_byte(out, 0x8F);
	gen_modrm_sib(out, 0, rm);
}

static __forceinline void gen_pushfd(uint8_t **out)
{
	gen_byte(out, 0x9C);
}

static __forceinline void gen_push_rm(uint8_t **out, struct modrm_rm_t rm)
{
	gen_byte(out, 0xFF);
	gen_modrm_sib(out, 6, rm);
}

static __forceinline void gen_push_imm32(uint8_t **out, uint32_t imm)
{
	gen_byte(out, 0x68);
	gen_dword(out, imm);
}

static __forceinline void gen_call(uint8_t **out, size_t dest)
{
	int32_t rel = (int32_t)(dest - (((size_t)*out) + 5));
	gen_byte(out, 0xE8);
	gen_dword(out, rel);
}

static __forceinline void gen_jmp(uint8_t **out, size_t dest)
{
	int32_t rel = (int32_t)(dest - (((size_t)*out) + 5));
	gen_byte(out, 0xE9);
	gen_dword(out, rel);
}

static __forceinline void gen_jcc(uint8_t **out, int cond, size_t dest)
{
	int32_t rel = (int32_t)(dest - (((size_t)*out) + 6));
	gen_byte(out, 0x0F);
	gen_byte(out, 0x80 + cond);
	gen_dword(out, rel);
}

struct dbt_block
{
	FORWARD_LIST_NODE(struct dbt_block);
	size_t pc;
	uint8_t *start;
};

#define DBT_OUT_ALIGN			16
#define DBT_BLOCK_HASH_BUCKETS	4096
#define DBT_BLOCK_MAXSIZE		1024 /* Maximum size of a translated basic block */
#define MAX_DBT_BLOCKS			(DBT_BLOCKS_SIZE / sizeof(struct dbt_block))
struct dbt_data
{
	FORWARD_LIST(struct dbt_block) block_hash[DBT_BLOCK_HASH_BUCKETS];
	struct dbt_block *blocks;
	int blocks_count;
	uint8_t *out, *end;
	/* Offsets for accessing thread local storage in fs:[.] */
	int tls_scratch_offset; /* scratch variable */
	int tls_gs_offset; /* gs value */
	int tls_gs_addr_offset; /* gs base address */
};

static struct dbt_data *const dbt = DBT_DATA_BASE;
static uint8_t *const dbt_cache = DBT_CACHE_BASE;

void dbt_init()
{
	log_info("Initializing dbt subsystem...\n");
	if (!VirtualAlloc(dbt, sizeof(struct dbt_data), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
		log_error("VirtualAlloc() for dbt_data failed.\n");
	if (!VirtualAlloc(DBT_BLOCKS_BASE, DBT_BLOCKS_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
		log_error("VirtualAlloc() for dbt_blocks failed.\n");
	if (!VirtualAlloc(dbt_cache, DBT_CACHE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
		log_error("VirtualAlloc() for dbt_cache failed.\n");
	dbt->blocks = DBT_BLOCKS_BASE;
	dbt->blocks_count = 0;
	dbt->out = dbt_cache;
	dbt->end = dbt_cache + DBT_CACHE_SIZE;

	int scratch_slot = tls_alloc();
	dbt->tls_scratch_offset = tls_slot_to_offset(scratch_slot);
	log_info("scratch slot: %d, offset: %p\n", scratch_slot, dbt->tls_scratch_offset);
	int gs_slot = tls_alloc();
	dbt->tls_gs_offset = tls_slot_to_offset(gs_slot);
	log_info("gs slot: %d, offset: %p\n", gs_slot, dbt->tls_gs_offset);
	int gs_addr_slot = tls_alloc();
	dbt->tls_gs_addr_offset = tls_slot_to_offset(gs_addr_slot);
	log_info("gs_addr slot: %d, offset: %p\n", gs_addr_slot, dbt->tls_gs_addr_offset);
	log_info("dbt subsystem initialized.\n");
}

void dbt_shutdown()
{
	VirtualFree(dbt, 0, MEM_RELEASE);
	VirtualFree(dbt_cache, 0, MEM_RELEASE);
}

static void dbt_flush()
{
	for (int i = 0; i < DBT_BLOCK_HASH_BUCKETS; i++)
		forward_list_init(&dbt->block_hash[i]);
	dbt->blocks_count = 0;
	dbt->out = dbt_cache;
	dbt->end = dbt_cache + DBT_CACHE_SIZE;
}

void dbt_reset()
{
	dbt_flush();
}

static int hash_block_pc(size_t pc)
{
	return (pc + (pc << 3) + (pc << 9)) % DBT_BLOCK_HASH_BUCKETS;
}

static struct dbt_block *alloc_block()
{
	if (dbt->blocks_count == MAX_DBT_BLOCKS || dbt->end - dbt->out < DBT_BLOCK_MAXSIZE)
		return NULL;
	return &dbt->blocks[dbt->blocks_count++];
}

static struct dbt_block *find_block(size_t pc)
{
	int bucket = hash_block_pc(pc);
	struct dbt_block *block, *prev;
	forward_list_iterate(&dbt->block_hash[bucket], prev, block)
		if (block->pc == pc)
			return block;
	return NULL;
}

static size_t dbt_get_direct_trampoline(size_t pc, size_t patch_addr)
{
	extern void dbt_find_direct_internal();

	struct dbt_block *cached_block = find_block(pc);
	if (cached_block)
		return cached_block->start;

	/* Not found in cache, create a stub */
	/* Caution: we must ensure that this stub fits in DBT_OUT_ALIGN(16) bytes */
	dbt->end -= DBT_OUT_ALIGN;
	uint8_t *out = dbt->end;
	gen_push_imm32(&out, patch_addr);
	gen_push_imm32(&out, pc);
	gen_jmp(&out, &dbt_find_direct_internal);
	return (size_t)dbt->end;
}

struct instruction_t
{
	int escape_0x0f;
	uint8_t opcode;
	uint8_t opsize_prefix, rep_prefix;
	int r;
	struct modrm_rm_t rm;
	int imm_bytes;
	struct instruction_desc *desc;
};

/* Find and return an unused register in an instruction, which can be used to hold temporary values */
static int find_unused_register(struct instruction_t *ins)
{
	/* Calculate used registers in this instruction */
	int used_regs = ins->desc->read_regs | ins->desc->write_regs;
	if (ins->r != -1)
		used_regs |= REG_MASK(ins->r);
	if (ins->rm.base != -1)
		used_regs |= REG_MASK(ins->rm.base);
	if (ins->rm.index != -1)
		used_regs |= REG_MASK(ins->rm.index);
#define TEST_REG(r) do { if ((used_regs & REG_MASK(r)) == 0) return r; } while (0)
	/* We really don't want to use esp or ebp as a temporary register */
	TEST_REG(0); /* Eax */
	TEST_REG(1); /* Ecx */
	TEST_REG(2); /* Edx */
	TEST_REG(3); /* Ebx */
	TEST_REG(6); /* Esi */
	TEST_REG(7); /* Edi */
#undef TEST_REG
	log_error("find_unused_register: No usable register found. There must be a bug in our implementation.\n");
	__debugbreak();
}

static struct dbt_block *dbt_translate(size_t pc)
{
	extern void dbt_find_indirect_internal();

	struct dbt_block *block = alloc_block();
	if (!block) /* The cache is full */
	{
		/* TODO: We may need to check this flush-all-on-full semantic when we add signal handling */
		dbt_flush();
		block = alloc_block(); /* We won't fail again */
	}
	block->pc = pc;
	block->start = ((size_t)dbt->out + DBT_OUT_ALIGN - 1) & -(size_t)DBT_OUT_ALIGN;

	uint8_t *code = (uint8_t *)pc;
	uint8_t *out = block->start;
	for (;;)
	{
		struct instruction_t ins;
		ins.rep_prefix = 0;
		ins.opsize_prefix = 0;
		/* Handle prefixes. According to x86 doc, they can appear in any order */
		for (;;)
		{
			ins.opcode = parse_byte(&code);
			/* TODO: Can we migrate this switch to a table driven approach? */
			switch (ins.opcode)
			{
			case 0xF0: /* LOCK */
				log_error("LOCK prefix not supported\n");
				__debugbreak();
				continue;

			case 0xF2: /* REPNE/REPNZ */
				ins.rep_prefix = 0xF2;
				continue;

			case 0xF3: /* REP/REPE/REPZ */
				ins.rep_prefix = 0xF3;
				continue;

			case 0x2E: /* CS segment override*/
				log_error("CS segment override not supported\n");
				__debugbreak();
				continue;

			case 0x36: /* SS segment override */
				log_error("SS segment override not supported\n");
				__debugbreak();
				continue;

			case 0x3E: /* DS segment override */
				log_error("DS segment override not supported\n");
				__debugbreak();
				continue;

			case 0x26: /* ES segment override */
				log_error("ES segment override not supported\n");
				__debugbreak();
				continue;

			case 0x64: /* FS segment override */
				log_error("FS segment override not supported\n");
				__debugbreak();
				continue;

			case 0x65: /* GS segment override */
				log_error("GS segment override not supported\n");
				__debugbreak();
				continue;

			case 0x66: /* Operand size prefix */
				ins.opsize_prefix = 0x66;
				continue;

			case 0x67: /* Address size prefix */
				log_error("Address size prefix not supported\n");
				__debugbreak();
				continue;
			}
			break;
		}

		/* Extract instruction descriptor */
		ins.escape_0x0f = 0;

		if (ins.opcode == 0x0F)
		{
			ins.escape_0x0f = 1;
			ins.opcode = parse_byte(&code);
			ins.desc = &two_byte_inst[ins.opcode];
		}
		else
			ins.desc = &one_byte_inst[ins.opcode];

		if (ins.desc->has_modrm)
			parse_modrm(&code, &ins.r, &ins.rm);
		
	inst_extension_reentry:
		ins.imm_bytes = ins.desc->imm_bytes;
		if (ins.imm_bytes == PREFIX_OPERAND_SIZE)
			ins.imm_bytes = ins.opsize_prefix? 2: 4;

		/* Translate instruction */
		switch (ins.desc->type)
		{
		case INST_TYPE_UNKNOWN: log_error("Unknown opcode.\n"); __debugbreak(); break;
		case INST_TYPE_INVALID: log_error("Invalid opcode.\n"); __debugbreak(); break;
		case INST_TYPE_PRIVILEGED: log_error("Privileged opcode.\n"); __debugbreak(); break;
		case INST_TYPE_UNSUPPORTED: log_error("Unsupported opcode.\n"); __debugbreak(); break;

		case INST_TYPE_EXTENSION:
		{
			ins.desc = &ins.desc->extension_table[ins.r];
			goto inst_extension_reentry;
		}

		case INST_TYPE_NORMAL:
		{
			/* TODO: Handle GS prefix */
			uint8_t *imm_start = code;
			code += ins.imm_bytes;

			if (ins.opsize_prefix)
				gen_byte(&out, ins.opsize_prefix);
			if (ins.rep_prefix)
				gen_byte(&out, ins.rep_prefix);
			if (ins.escape_0x0f)
				gen_byte(&out, 0x0f);
			gen_byte(&out, ins.opcode);
			if (ins.desc->has_modrm)
				gen_modrm_sib(&out, ins.r, ins.rm);
			gen_copy(&out, imm_start, ins.imm_bytes);
			break;
		}

		case INST_CALL_DIRECT:
		{
			int32_t rel = parse_rel(&code, ins.imm_bytes);
			size_t dest = (size_t)code + rel;
			gen_push_imm32(&out, (size_t)code);
			size_t patch_addr = (size_t)out + 1;
			gen_jmp(&out, dbt_get_direct_trampoline(dest, patch_addr));
			goto end_block;
		}

		case INST_CALL_INDIRECT:
		{
			/* TODO: Bad codegen for `call esp', although should not be used in practice */
			gen_push_imm32(&out, (size_t)code);
			if (ins.rm.base == 4) /* ESP-related address */
				ins.rm.disp += 4;
			gen_push_rm(&out, ins.rm);
			gen_jmp(&out, &dbt_find_indirect_internal);
			goto end_block;
		}

		case INST_RET:
		{
			gen_jmp(&out, &dbt_find_indirect_internal);
			goto end_block;
		}

		case INST_RETN:
		{
			int count = parse_word(&code);
			/* pop [esp - 4 + count] */
			/* esp increases before pop operation */
			struct modrm_rm_t rm = modrm_rm_mreg(4, count - 4);
			gen_pop_rm(&out, rm);
			/* lea esp, [esp - 4 + count] */
			gen_lea(&out, 4, rm);
			gen_jmp(&out, &dbt_find_indirect_internal);
			goto end_block;
		}

		case INST_JMP_DIRECT:
		{
			int32_t rel = parse_rel(&code, ins.imm_bytes);
			size_t dest = (size_t)code + rel;
			size_t patch_addr = (size_t)out + 1;
			gen_jmp(&out, dbt_get_direct_trampoline(dest, patch_addr));
			goto end_block;
		}

		case INST_JMP_INDIRECT:
		{
			gen_push_rm(&out, ins.rm);
			gen_jmp(&out, &dbt_find_indirect_internal);
			goto end_block;
		}

		case INST_JCC + 0:
		case INST_JCC + 1:
		case INST_JCC + 2:
		case INST_JCC + 3:
		case INST_JCC + 4:
		case INST_JCC + 5:
		case INST_JCC + 6:
		case INST_JCC + 7:
		case INST_JCC + 8:
		case INST_JCC + 9:
		case INST_JCC + 10:
		case INST_JCC + 11:
		case INST_JCC + 12:
		case INST_JCC + 13:
		case INST_JCC + 14:
		case INST_JCC + 15:
		{
			int cond = GET_JCC_COND(ins.desc->type);
			int32_t rel = parse_rel(&code, ins.imm_bytes);
			size_t dest0 = (size_t)code + rel; /* Branch taken */
			size_t dest1 = (size_t)code; /* Branch not taken */
			size_t patch_addr0 = (size_t)out + 2;
			gen_jcc(&out, cond, dbt_get_direct_trampoline(dest0, patch_addr0));
			size_t patch_addr1 = (size_t)out + 1;
			gen_jmp(&out, dbt_get_direct_trampoline(dest1, patch_addr1));
			goto end_block;
		}

		case INST_JCC_REL8:
		{
			int32_t rel = parse_rel(&code, ins.imm_bytes);
			size_t dest0 = (size_t)code + rel; /* Branch taken */
			size_t dest1 = (size_t)code; /* Branch not taken */
			/* LOOP, LOOPE, LOOPNE, JCXZ, JECXZ, JRCXZ */
			/* op $+2 */
			gen_byte(&out, ins.opcode);
			gen_byte(&out, 2); /* sizeof(jmp rel8) */
			/* jmp $+5 */
			gen_byte(&out, 0xEB);
			gen_byte(&out, 5); /* sizeof(jmp rel32) */
			size_t patch_addr0 = (size_t)out + 1;
			gen_jmp(&out, dbt_get_direct_trampoline(dest0, patch_addr0));
			size_t patch_addr1 = (size_t)out + 1;
			gen_jmp(&out, dbt_get_direct_trampoline(dest1, patch_addr1));
			goto end_block;
		}

		case INST_INT:
		{
			extern void syscall_handler();
			uint8_t id = parse_byte(&code);
			if (id != 0x80)
			{
				log_error("INT 0x%x not supported.\n", id);
				__debugbreak();
			}
			gen_call(&out, &syscall_handler);
			break;
		}

		case INST_MOV_FROM_SEG:
		{
			if (ins.r != 5) /* GS */
			{
				log_error("mov from segment selectors other than GS not supported.\n");
				__debugbreak();
			}
			int temp_reg = find_unused_register(&ins, 0);
			/* mov fs:[scratch], temp_reg */
			gen_fs_prefix(&out);
			gen_mov_rm_r_32(&out, modrm_rm_disp(dbt->tls_scratch_offset), temp_reg);

			/* mov temp_reg, fs:[gs] */
			gen_fs_prefix(&out);
			gen_mov_r_rm_32(&out, temp_reg, modrm_rm_disp(dbt->tls_gs_offset));

			/* mov |rm|, temp_reg */
			gen_mov_rm_r_32(&out, ins.rm, temp_reg);

			/* mov temp_reg, fs:[scratch] */
			gen_fs_prefix(&out);
			gen_mov_r_rm_32(&out, temp_reg, modrm_rm_disp(dbt->tls_scratch_offset));
			break;
		}

		case INST_MOV_TO_SEG:
		{
			if (ins.r != 5) /* GS */
			{
				log_error("mov to segment selector other than GS not supported.\n");
				__debugbreak();
			}
			int temp_reg = find_unused_register(&ins);
			/* mov fs:[scratch], temp_reg */
			gen_fs_prefix(&out);
			gen_mov_rm_r_32(&out, modrm_rm_disp(dbt->tls_scratch_offset), temp_reg);

			/* mov temp_reg, |rm| */
			gen_mov_r_rm_32(&out, temp_reg, ins.rm);

			/* This is very ugly and inefficient, but anyway this instruction should not be used very often */
			gen_pushfd(&out);

			/* mov fs:[gs], temp_reg */
			gen_fs_prefix(&out);
			gen_mov_rm_r_32(&out, modrm_rm_disp(dbt->tls_gs_offset), temp_reg);

			/* call tls_slot_to_offset() to get the offset */
			gen_shr_rm_32(&out, modrm_rm_reg(temp_reg), 3);
			gen_push_rm(&out, modrm_rm_reg(0));
			gen_push_rm(&out, modrm_rm_reg(1));
			gen_push_rm(&out, modrm_rm_reg(2));
			gen_push_rm(&out, modrm_rm_reg(temp_reg));
			gen_call(&out, &tls_slot_to_offset);
			
			/* mov temp_reg, fs:eax */
			gen_fs_prefix(&out);
			gen_mov_r_rm_32(&out, temp_reg, modrm_rm_mreg(0, 0));
			/* mov fs:[gs_addr], temp_reg */
			gen_fs_prefix(&out);
			gen_mov_rm_r_32(&out, modrm_rm_disp(dbt->tls_gs_addr_offset), temp_reg);

			/* Clean up */
			gen_lea(&out, 4, modrm_rm_mreg(4, 4));
			gen_pop_rm(&out, modrm_rm_reg(2));
			gen_pop_rm(&out, modrm_rm_reg(1));
			gen_pop_rm(&out, modrm_rm_reg(0));

			gen_popfd(&out);

			/* mov temp_reg, fs:[scratch] */
			gen_fs_prefix(&out);
			gen_mov_r_rm_32(&out, temp_reg, modrm_rm_disp(dbt->tls_scratch_offset));
			break;
		}
		}
		continue;

	end_block:
		break;
	}
	dbt->out = out;
	return block;
}

size_t dbt_find_next(size_t pc)
{
	int bucket = hash_block_pc(pc);
	struct dbt_block *block, *prev;
	forward_list_iterate(&dbt->block_hash[bucket], prev, block)
		if (block->pc == pc)
			return block->start;

	/* Block not found, translate it now */
	block = dbt_translate(pc);
	forward_list_add(&dbt->block_hash[bucket], block);
	return block->start;
}

size_t dbt_find_direct(size_t pc, size_t patch_addr)
{
	/* Translate or generate the block */
	size_t block_start = dbt_find_next(pc);
	/* Patch the jmp/call address so we don't need to repeat work again */
	*(size_t*)patch_addr = (intptr_t)(block_start - (patch_addr + 4)); /* Relative address */
	return block_start;
}

void dbt_run(size_t pc, size_t sp)
{
	size_t entrypoint = dbt_find_next(pc);
	extern __declspec(noreturn) void dbt_run_internal(size_t pc, size_t sp);
	log_info("dbt: Calling into application code generated at %p, (original: pc: %p, sp: %p)\n", entrypoint, pc, sp);
	dbt_run_internal(entrypoint, sp);
}
