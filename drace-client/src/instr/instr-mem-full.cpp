/*
 * DRace, a dynamic data race detector
 *
 * Copyright 2018 Siemens AG
 *
 * Authors:
 *   Felix Moessbauer <felix.moessbauer@siemens.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "memory-tracker.h"

using namespace drace;
/*
* Inserts a jump if a flush is pending
*/
void MemoryTracker::insert_jmp_on_flush(void *drcontext, instrlist_t *ilist, instr_t *where,
	reg_id_t regxcx, reg_id_t regtls,
	instr_t *call_flush)
{
	instr_t *instr;
	opnd_t   opnd1, opnd2;

	//if(flush)
	// jmp .clean_call
	//

	opnd1 = opnd_create_reg(reg_resize_to_opsz(regxcx, OPSZ_1));
	opnd2 = OPND_CREATE_MEM8(regtls, offsetof(per_thread_t, no_flush));
	instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	opnd1 = opnd_create_instr(call_flush);
	instr = INSTR_CREATE_jecxz(drcontext, opnd1);
	instrlist_meta_preinsert(ilist, where, instr);
}

/* insert inline code to add a memory reference info entry into the buffer */
void MemoryTracker::instrument_mem_full(void *drcontext, instrlist_t *ilist, instr_t *where,
	opnd_t ref, bool write)
{
	/*
	* instrument_mem is called whenever a memory reference is identified.
	* It inserts code before the memory reference to to fill the memory buffer
	* and jump to our own code cache to call the clean_call when the buffer is full.
	*/
	instr_t *instr;
	opnd_t   opnd1, opnd2;
	reg_id_t reg1, reg2, reg3;
	app_pc pc;

	/* Steal two scratch registers.
	* reg2 must be ECX or RCX for jecxz.
	*/
	if (drreg_reserve_register(drcontext, ilist, where, &allowed_xcx, &reg2) !=
		DRREG_SUCCESS ||
		drreg_reserve_register(drcontext, ilist, where, NULL, &reg1) != DRREG_SUCCESS ||
		drreg_reserve_register(drcontext, ilist, where, NULL, &reg3) != DRREG_SUCCESS)
	{
		DR_ASSERT(false); /* cannot recover */
		return;
	}

	/* Create ASM lables */
	instr_t *restore = INSTR_CREATE_label(drcontext);
	instr_t *call = INSTR_CREATE_label(drcontext);
	instr_t *call_flush = INSTR_CREATE_label(drcontext);
	instr_t *call_noflush = INSTR_CREATE_label(drcontext);
	instr_t *after_flush = INSTR_CREATE_label(drcontext);

	/* use drutil to get mem address */
	drutil_insert_get_mem_addr(drcontext, ilist, where, ref, reg1, reg2);

	/* The following assembly performs the following instructions
	* if (disabled)
	*   jmp .restore;
	* if (flush)
	*   jmp .call
	* buf_ptr->write = write;
	* buf_ptr->addr  = addr;
	* buf_ptr->size  = size;
	* buf_ptr->pc    = pc;
	* buf_ptr++;
	* if (buf_ptr >= buf_end_ptr)
	*    clean_call();
	* .restore
	*/

	/* Precondition:
	reg1: memory address of access
	reg3: wiped / unknown state
	*/
	drmgr_insert_read_tls_field(drcontext, tls_idx, ilist, where, reg3);

	/* Jump if tracing is disabled */
	/* load enabled flag into reg2 */
	opnd1 = opnd_create_reg(reg_resize_to_opsz(reg2, OPSZ_1));
	opnd2 = OPND_CREATE_MEM8(reg3, offsetof(per_thread_t, enabled));
	instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* Jump if (E|R)CX is 0 */
	opnd1 = opnd_create_instr(restore);
	instr = INSTR_CREATE_jecxz(drcontext, opnd1);
	instrlist_meta_preinsert(ilist, where, instr);

	/* Jump if flush is pending, finally return to .after_flush */
	insert_jmp_on_flush(drcontext, ilist, where, reg2, reg3, call_flush);

	/* ==== .after_flush ==== */
	instrlist_meta_preinsert(ilist, where, after_flush);

	/* Load data->buf_ptr into reg2 */
	opnd1 = opnd_create_reg(reg2);
	opnd2 = OPND_CREATE_MEMPTR(reg3, offsetof(per_thread_t, buf_ptr));
	instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* Move write/read to write field */
	opnd1 = OPND_CREATE_MEM8(reg2, offsetof(mem_ref_t, write));
	opnd2 = OPND_CREATE_INT8(write);
	instr = INSTR_CREATE_mov_imm(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* Store address in memory ref */
	opnd1 = OPND_CREATE_MEMPTR(reg2, offsetof(mem_ref_t, addr));
	opnd2 = opnd_create_reg(reg1);
	instr = INSTR_CREATE_mov_st(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* Store size in memory ref */
	opnd1 = OPND_CREATE_MEM32(reg2, offsetof(mem_ref_t, size));
	/* drutil_opnd_mem_size_in_bytes handles OP_enter */
	opnd2 = OPND_CREATE_INT32(drutil_opnd_mem_size_in_bytes(ref, where));
	instr = INSTR_CREATE_mov_st(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	///* Store pc in memory ref */
	pc = instr_get_app_pc(where);
	///* For 64-bit, we can't use a 64-bit immediate so we split pc into two halves.
	//* We could alternatively load it into reg1 and then store reg1.
	//* We use a convenience routine that does the two-step store for us.
	//*/
	opnd1 = OPND_CREATE_MEMPTR(reg2, offsetof(mem_ref_t, pc));
	instrlist_insert_mov_immed_ptrsz(drcontext, (ptr_int_t)pc, opnd1,
		ilist, where, NULL, NULL);

	/* Increment reg value by pointer size using lea instr */
	opnd1 = opnd_create_reg(reg2);
	opnd2 = opnd_create_base_disp(reg2, DR_REG_NULL, 0,
		sizeof(mem_ref_t),
		OPSZ_lea);
	instr = INSTR_CREATE_lea(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* Update the data->buf_ptr */
	opnd1 = OPND_CREATE_MEMPTR(reg3, offsetof(per_thread_t, buf_ptr));
	opnd2 = opnd_create_reg(reg2);
	instr = INSTR_CREATE_mov_st(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* we use lea + jecxz trick for better performance
	* lea and jecxz won't disturb the eflags, so we won't insert
	* code to save and restore application's eflags.
	*/
	/* lea [reg2 - buf_end] => reg2 */
	opnd1 = opnd_create_reg(reg1);
	opnd2 = OPND_CREATE_MEMPTR(reg3, offsetof(per_thread_t, buf_end));
	instr = INSTR_CREATE_mov_ld(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);
	opnd1 = opnd_create_reg(reg2);
	opnd2 = opnd_create_base_disp(reg1, reg2, 1, 0, OPSZ_lea);
	instr = INSTR_CREATE_lea(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* jecxz call */
	opnd1 = opnd_create_instr(call_noflush);
	instr = INSTR_CREATE_jecxz(drcontext, opnd1);
	instrlist_meta_preinsert(ilist, where, instr);

	/* jump restore to skip clean call */
	opnd1 = opnd_create_instr(restore);
	instr = INSTR_CREATE_jmp(drcontext, opnd1);
	instrlist_meta_preinsert(ilist, where, instr);

	/* ==== .call_flush ==== */
	instrlist_meta_preinsert(ilist, where, call_flush);

	/* mov restore DR_REG_XCX */
	opnd1 = opnd_create_reg(reg2);
	/* this is the return address for jumping back from lean procedure */
	opnd2 = opnd_create_instr(after_flush);
	/* We could use instrlist_insert_mov_instr_addr(), but with a register
	* destination we know we can use a 64-bit immediate.
	*/
	instr = INSTR_CREATE_mov_imm(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	opnd1 = opnd_create_instr(call);
	instr = INSTR_CREATE_jmp(drcontext, opnd1);
	instrlist_meta_preinsert(ilist, where, instr);

	/* ==== .call_noflush ==== */

	/* Prepare return address of code cache */
	instrlist_meta_preinsert(ilist, where, call_noflush);
	/* mov restore DR_REG_XCX */
	opnd1 = opnd_create_reg(reg2);
	/* this is the return address for jumping back from lean procedure */
	opnd2 = opnd_create_instr(restore);
	/* We could use instrlist_insert_mov_instr_addr(), but with a register
	* destination we know we can use a 64-bit immediate.
	*/
	instr = INSTR_CREATE_mov_imm(drcontext, opnd1, opnd2);
	instrlist_meta_preinsert(ilist, where, instr);

	/* ==== .call ==== */
	/* clean call */
	/* We jump to lean procedure which performs full context switch and
	* clean call invocation. This is to reduce the code cache size.
	*/
	instrlist_meta_preinsert(ilist, where, call);
	/* jmp cc_flush */
	opnd1 = opnd_create_pc(cc_flush);
	instr = INSTR_CREATE_jmp(drcontext, opnd1);

	instrlist_meta_preinsert(ilist, where, instr);

	/* ==== .restore ==== */
	/* Restore scratch registers */
	instrlist_meta_preinsert(ilist, where, restore);

	if (drreg_unreserve_register(drcontext, ilist, where, reg1) != DRREG_SUCCESS ||
		drreg_unreserve_register(drcontext, ilist, where, reg2) != DRREG_SUCCESS || 
		drreg_unreserve_register(drcontext, ilist, where, reg3) != DRREG_SUCCESS)
		DR_ASSERT(false);
}
