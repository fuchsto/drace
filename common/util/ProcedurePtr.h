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

#include <type_traits>

namespace util {
    /**
    * Encapsulates a pointer to a native function
    */
    class ProcedurePtr {
    public:
        explicit ProcedurePtr(void * ptr) : _ptr(ptr) {}

        template <typename T, typename = std::enable_if_t<std::is_function_v<T>>>
        operator T *() const {
            return reinterpret_cast<T *>(_ptr);
        }

    private:
        void * _ptr;
    };
}
