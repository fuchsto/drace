#pragma once

#include <dr_api.h>

/* Aligned Buffer, allocated in DR's thread local storage of instantiator */
template<typename T, size_t alignment>
class AlignedBuffer {
	/* unaligned buffer begin */
	void *   _mem;

	/* size of the buffer in bytes */
	size_t   _size_in_bytes{ 0 };

public:
	using self_t = AlignedBuffer<T, alignment>;
	T * array;

public:
	AlignedBuffer() = default;
	AlignedBuffer(const self_t & other) = delete;
	AlignedBuffer(self_t && other) = default;

	self_t & operator= (const self_t & other) = delete;
	self_t & operator= (self_t && other) = default;

	explicit AlignedBuffer(const size_t & capacity) {
		allocate(capacity);
	}

	/* Clears the old buffer and allocates a new one with the specified capacity */
	void resize(size_t capacity) {
		deallocate();
		allocate(capacity);
	}

	~AlignedBuffer() {
		deallocate();
	}

private:
	void allocate(size_t capacity) {
		void* drcontext = dr_get_current_drcontext();
		_size_in_bytes = capacity * sizeof(T) + alignment;
		_mem = (void*)dr_thread_alloc(drcontext, _size_in_bytes);
		array = (T*)std::align(alignment, capacity, _mem, _size_in_bytes);
		DR_ASSERT(((uint64_t)array % alignment) == 0);
	}

	void deallocate() {
		if (_size_in_bytes != 0) {
			void* drcontext = dr_get_current_drcontext();
			dr_thread_free(drcontext, _mem, _size_in_bytes);
			_size_in_bytes = 0;
			array = nullptr;
		}
	}
};
