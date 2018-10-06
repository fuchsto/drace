#include "globals.h"
#include "util.h"
#include "function-wrapper.h"
#include "memory-tracker.h"
#include "symbols.h"
#include "statistics.h"
#include "detector_if.h"

#include <dr_api.h>
#include <drmgr.h>
#include <drwrap.h>

namespace funwrap {

	void event::beg_excl_region(per_thread_t * data) {
		// We do not flush here, as in disabled state no
		// refs are recorded
		//memory_tracker->process_buffer();
		LOG_TRACE(data->tid, "Begin excluded region");
		data->enabled = false;
		data->event_cnt++;
	}

	void event::end_excl_region(per_thread_t * data) {
		LOG_TRACE(data->tid, "End excluded region");
		if (data->event_cnt == 1) {
			//memory_tracker->clear_buffer();
			data->enabled = true;
		}
		data->event_cnt--;
	}

	// TODO: On Linux size is arg 0
	void event::alloc_pre(void *wrapctx, void **user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);

		// Save allocate size to user_data
		// we use the pointer directly to avoid an allocation
		//per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		*user_data = drwrap_get_arg(wrapctx, 2);
	}

	void event::alloc_post(void *wrapctx, void *user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		void * retval = drwrap_get_retval(wrapctx);
		void * pc = drwrap_get_func(wrapctx);

		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		MemoryTracker::flush_all_threads(data);
		detector::allocate(data->tid, pc, retval, reinterpret_cast<size_t>(user_data), data->detector_data);
	}

	// TODO: On Linux addr is arg 0
	void event::free_pre(void *wrapctx, void **user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		void * addr = drwrap_get_arg(wrapctx, 2);

		MemoryTracker::flush_all_threads(data);
		detector::deallocate(data->tid, addr, data->detector_data);
	}

	void event::free_post(void *wrapctx, void *user_data) {
		//app_pc drcontext = drwrap_get_drcontext(wrapctx);
		//per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);

		//end_excl_region(data);
		//dr_fprintf(STDERR, "<< [%i] post free\n", data->tid);
	}

	void event::thread_creation(void *wrapctx, void **user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		beg_excl_region(data);

		th_start_pending.store(true);
		LOG_INFO(data->tid, "setup new thread");
	}
	void event::thread_handover(void *wrapctx, void *user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		end_excl_region(data);
		// Enable recently started thread
		auto last_th = last_th_start.load(std::memory_order_relaxed);
		// TLS is already updated, hence read lock is sufficient
		dr_rwlock_read_lock(tls_rw_mutex);
		if (TLS_buckets.count(last_th) == 1) {
			auto & other_tls = TLS_buckets[last_th];
			if (other_tls->event_cnt == 0)
				TLS_buckets[last_th]->enabled = true;
		}
		dr_rwlock_read_unlock(tls_rw_mutex);
		LOG_INFO(data->tid, "new thread created: %i", last_th_start.load());
	}

	void event::thread_pre_sys(void *wrapctx, void **user_data) {
	}
	void event::thread_post_sys(void *wrapctx, void *user_data) {
		//app_pc drcontext = drwrap_get_drcontext(wrapctx);
		//per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		dr_rwlock_read_lock(tls_rw_mutex);
		auto other_th = last_th_start.load(std::memory_order_acquire);
		// There are some spurious failures where the thread init event
		// is not called but the system call has already returned
		// Hence, skip the fork here and rely on fallback-fork in
		// analyze_access
		if (TLS_buckets.count(other_th) == 1) {
			auto other_tls = TLS_buckets[other_th];
			//MemoryTracker::flush_all_threads(data, false);
			//detector::fork(dr_get_thread_id(drcontext), other_tls->tid, &(other_tls->detector_data));
		}
		dr_rwlock_read_unlock(tls_rw_mutex);
	}

	void event::begin_excl(void *wrapctx, void **user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		beg_excl_region(data);
	}

	void event::end_excl(void *wrapctx, void *user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		end_excl_region(data);
	}

	void event::dotnet_enter(void *wrapctx, void **user_data) { }
	void event::dotnet_leave(void *wrapctx, void *user_data) { }

	// --------------------------------------------------------------------------------------
	// ------------------------------------- Mutex Events -----------------------------------
	// --------------------------------------------------------------------------------------

	void event::prepare_and_aquire(
		void* wrapctx,
		void* mutex,
		bool write,
		bool trylock)
	{
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		if (params.exclude_master && data->tid == runtime_tid)
			return;

		if (trylock) {
			int retval = (int)drwrap_get_retval(wrapctx);
			LOG_TRACE(data->tid, "Try Aquire %p, res: %i", mutex, retval);
			//dr_printf("[%.5i] Try Aquire %p, ret %i\n", data->tid, mutex, retval);
			// If retval == 0, mtx acquired
			// skip otherwise
			if (retval != 0)
				return;
		}
		LOG_TRACE(data->tid, "Aquire %p : %s", mutex, symbol_table->get_symbol_info(drwrap_get_func(wrapctx)).sym_name.c_str());

		// To avoid deadlock in flush-waiting spinlock,
		// acquire / release must not occur concurrently
		uint64_t cnt = ++(data->mutex_book[(uint64_t)mutex]);

		LOG_TRACE(data->tid, "Mutex book size: %i, count: %i, mutex: %p\n", data->mutex_book.size(), cnt, mutex);

		//dr_mutex_lock(th_mutex);
		MemoryTracker::flush_all_threads(data);
		detector::acquire(data->tid, mutex, cnt, write, false, data->detector_data);
		//dr_mutex_unlock(th_mutex);

		data->stats->mutex_ops++;
	}

	void event::prepare_and_release(
		void* wrapctx,
		bool write)
	{
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		if (params.exclude_master && data->tid == runtime_tid)
			return;

		void* mutex = drwrap_get_arg(wrapctx, 0);

		if (!data->mutex_book.count((uint64_t)mutex)) {
			LOG_TRACE(data->tid, "Mutex Error %p at : %s", mutex, symbol_table->get_symbol_info(drwrap_get_func(wrapctx)).sym_name.c_str());
			// mutex not in book
			return;
		}
		auto & cnt = --(data->mutex_book[(uint64_t)mutex]);
		if (cnt == 0) {
			data->mutex_book.erase((uint64_t)mutex);
		}

		// To avoid deadlock in flush-waiting spinlock,
		// acquire / release must not occur concurrently
		//dr_mutex_lock(th_mutex);
		MemoryTracker::flush_all_threads(data);
		LOG_TRACE(data->tid, "Release %p : %s", mutex, symbol_table->get_symbol_info(drwrap_get_func(wrapctx)).sym_name.c_str());
		detector::release(data->tid, mutex, write, data->detector_data);
		//dr_mutex_unlock(th_mutex);
	}

	void event::get_arg(void *wrapctx, OUT void **user_data) {
		*user_data = drwrap_get_arg(wrapctx, 0);
	}

	void event::get_arg_dotnet(void *wrapctx, OUT void **user_data) {
		LOG_INFO(-1, "Hit Whatever with arg %p", drwrap_get_arg(wrapctx, 0));
		*user_data = drwrap_get_arg(wrapctx, 0);
	}

	void event::mutex_lock(void *wrapctx, void *user_data) {
		prepare_and_aquire(wrapctx, user_data, true, false);
	}

	void event::mutex_trylock(void *wrapctx, void *user_data) {
		prepare_and_aquire(wrapctx, user_data, true, true);
	}

	void event::mutex_unlock(void *wrapctx, OUT void **user_data) {
		prepare_and_release(wrapctx, true);
	}

	void event::recmutex_lock(void *wrapctx, void *user_data) {
		// TODO: Check recursive parameter
		event::prepare_and_aquire(wrapctx, user_data, true, false);
	}

	void event::recmutex_trylock(void *wrapctx, void *user_data) {
		prepare_and_aquire(wrapctx, user_data, true, true);
	}

	void event::recmutex_unlock(void *wrapctx, OUT void **user_data) {
		prepare_and_release(wrapctx, true);
	}

	void event::mutex_read_lock(void *wrapctx, void *user_data) {
		prepare_and_aquire(wrapctx, user_data, false, false);
	}

	void event::mutex_read_trylock(void *wrapctx, void *user_data) {
		prepare_and_aquire(wrapctx, user_data, false, true);
	}

	void event::mutex_read_unlock(void *wrapctx, OUT void **user_data) {
		prepare_and_release(wrapctx, false);
	}

#ifdef WINDOWS
	void event::wait_for_single_obj(void *wrapctx, void *mutex) {
		// https://docs.microsoft.com/en-us/windows/desktop/api/synchapi/nf-synchapi-waitforsingleobject

		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		LOG_TRACE(data->tid, "waitForSingleObject: %p\n", mutex);

		if (params.exclude_master && data->tid == runtime_tid)
			return;

		DWORD retval = (DWORD)drwrap_get_retval(wrapctx);
		if (retval != WAIT_OBJECT_0) return;

		LOG_TRACE(data->tid, "waitForSingleObject: %p (Success)", mutex);

		uint64_t cnt = ++(data->mutex_book[(uint64_t)mutex]);
		MemoryTracker::flush_all_threads(data);
		detector::acquire(data->tid, mutex, cnt, 1, false, data->detector_data);
		data->stats->mutex_ops++;
	}

	void event::wait_for_mo_getargs(void *wrapctx, OUT void **user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DR_ASSERT(nullptr != data);

		wfmo_args_t * args = (wfmo_args_t*)dr_thread_alloc(drcontext, sizeof(wfmo_args_t));

		args->ncount = (DWORD)drwrap_get_arg(wrapctx, 0);
		args->handles = (const HANDLE*)drwrap_get_arg(wrapctx, 1);
		args->waitall = (BOOL)drwrap_get_arg(wrapctx, 2);

		LOG_TRACE(data->tid, "waitForMultipleObjects: %u, %i", args->ncount, args->waitall);

		*user_data = (void*)args;
	}

	/* WaitForMultipleObjects Windows API call (experimental) */
	void event::wait_for_mult_obj(void *wrapctx, void *user_data) {
		app_pc drcontext = drwrap_get_drcontext(wrapctx);
		per_thread_t * data = (per_thread_t*)drmgr_get_tls_field(drcontext, tls_idx);
		DWORD retval = (DWORD)drwrap_get_retval(wrapctx);

		wfmo_args_t * info = (wfmo_args_t*)user_data;

		MemoryTracker::flush_all_threads(data);
		if (info->waitall && (retval == WAIT_OBJECT_0)) {
			LOG_TRACE(data->tid, "waitForMultipleObjects:finished all");
			for (DWORD i = 0; i < info->ncount; ++i) {
				HANDLE mutex = info->handles[i];
				uint64_t cnt = ++(data->mutex_book[(uint64_t)mutex]);
				detector::acquire(data->tid, (void*)mutex, cnt, true, false, data->detector_data);
				data->stats->mutex_ops++;
			}
		}
		if (!info->waitall) {
			if (retval <= (WAIT_OBJECT_0 + info->ncount - 1)) {
				HANDLE mutex = info->handles[retval - WAIT_OBJECT_0];
				LOG_TRACE(data->tid, "waitForMultipleObjects:finished one: %p", mutex);
				uint64_t cnt = ++(data->mutex_book[(uint64_t)mutex]);
				detector::acquire(data->tid, (void*)mutex, cnt, true, false, data->detector_data);
				data->stats->mutex_ops++;
			}
		}

		dr_thread_free(drcontext, user_data, sizeof(wfmo_args_t));
	}
#endif
} // namespace funwrap