#pragma once

#include "module-tracker.h"

#include <dr_api.h>
#include <drmgr.h>
#include <drreg.h>
#include <drutil.h>
#include <dr_tools.h>

#include <atomic>
#include <memory>
#include <random>

// DR somewere defines max()
#undef max

class MemoryTracker {
public:
	/* Single memory reference */
	struct mem_ref_t {
		bool  write;
		void *addr;
		size_t size;
		app_pc pc;
	};

	/* Maximum number of references between clean calls */
	static constexpr int MAX_NUM_MEM_REFS = 128;
	static constexpr int MEM_BUF_SIZE = sizeof(mem_ref_t) * MAX_NUM_MEM_REFS;

	/* aggregate frequent pc's on this granularity (bytes)*/
	static constexpr unsigned HIST_PC_RES = 8 * 128;
	/* update code-cache after this number of flushes */
	static constexpr unsigned CC_UPDATE_PERIOD = 1024 * 64;

	std::atomic<int> flush_active{ false };

private:
	size_t page_size;

	/* Code Caches */
	app_pc cc_flush;

	/* Number of instrumented calls, used for sampling
	 * \Warning This number is racily incremented by design.
	 *          Hence, use the largest native type to avoid partial loads
	 */
	unsigned long long instrum_count{ 0 };

	/* XCX registers */
	drvector_t allowed_xcx;

	ModuleCache mc;

	// fast random numbers for sampling
	std::mt19937 _prng;
	const std::mt19937::result_type _prng_border;

	static const std::mt19937::result_type _max_value = decltype(_prng)::max();

public:

	MemoryTracker();
	~MemoryTracker();

	static void process_buffer(void);
	static void clear_buffer(void);
	static void analyze_access(per_thread_t * data);
	static void flush_all_threads(per_thread_t * data, bool self = true, bool flush_external = true);

	// Events
	void event_thread_init(void *drcontext);

	void event_thread_exit(void *drcontext);

	/* We transform string loops into regular loops so we can more easily
	* monitor every memory reference they make.
	*/
	dr_emit_flags_t event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating);

	dr_emit_flags_t event_app_analysis(void * drcontext, void * tag, instrlist_t * bb, bool for_trace, bool translating, OUT void ** user_data);
	/* Instrument application instructions */
	dr_emit_flags_t event_app_instruction(void *drcontext, void *tag, instrlist_t *bb,
		instr_t *instr, bool for_trace,
		bool translating, void *user_data);

	/* Returns true if this reference should be sampled */
	static inline bool sample_ref() {
		return (params.sampling_rate == 1 || (memory_tracker->_prng() < memory_tracker->_prng_border));
	}

	/* Update the code cache and remove items where the instrumentation should change.
	 * We only consider traces, as other parts are not performance critical
	 */
	static void update_cache(per_thread_t * data);

	static bool pc_in_freq(per_thread_t * data, void* bb);

private:

	void code_cache_init(void);
	void code_cache_exit(void);

	// Instrumentation
	/* Inserts a jump to clean call if a flush is pending */
	void MemoryTracker::insert_jmp_on_flush(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t reg2, instr_t *call_flush);

	/* Instrument all memory accessing instructions */
	void instrument_mem_full(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t ref, bool write);
	void instrument_mem_fast(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t ref, bool write);

	inline void instrument_mem(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t ref, bool write) {
		if (params.fastmode) {
			instrument_mem_fast(drcontext, ilist, where, ref, write);
		}
		else {
			instrument_mem_full(drcontext, ilist, where, ref, write);
		}
	}
};

extern std::unique_ptr<MemoryTracker> memory_tracker;

static inline dr_emit_flags_t instr_event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating)
{
	return memory_tracker->event_bb_app2app(drcontext, tag, bb, for_trace, translating);
}

static inline dr_emit_flags_t instr_event_app_analysis(void * drcontext, void * tag, instrlist_t * bb, bool for_trace, bool translating, OUT void ** user_data)
{
	return memory_tracker->event_app_analysis(drcontext, tag, bb, for_trace, translating, user_data);
}

static inline dr_emit_flags_t instr_event_app_instruction(void *drcontext, void *tag, instrlist_t *bb,
	instr_t *instr, bool for_trace,
	bool translating, void *user_data)
{
	return memory_tracker->event_app_instruction(drcontext, tag, bb, instr, for_trace, translating, user_data);
}
