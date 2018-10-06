#pragma once

#include "ipc/SyncSHMDriver.h"
#include <iostream>

#include <dr_api.h>

namespace ipc {
	/* Provides methods for synchronized access to a SyncSHMDriver */
	template<bool SENDER, bool NOTHROW = true>
	class MtSyncSHMDriver : public SyncSHMDriver<SENDER, NOTHROW> {
		void * _mx;

	public:
		MtSyncSHMDriver(const char* shmkey, bool create)
			: SyncSHMDriver(shmkey, create)
		{
			_mx = dr_mutex_create();
		}

		~MtSyncSHMDriver() {
			dr_mutex_destroy(_mx);
		}

		inline void lock() {
			dr_mutex_lock(_mx);
		}

		inline void unlock() {
			dr_mutex_unlock(_mx);
		}
	};
}