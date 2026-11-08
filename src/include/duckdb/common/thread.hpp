//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/thread.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#ifndef DUCKDB_NO_THREADS
#include <thread>
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {
using std::thread;
using thread_id = std::thread::id;

} // namespace duckdb

#else
using thread_id = uint64_t;
#endif

#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

class ClientContext;

struct ThreadUtil {
	static void SleepMs(idx_t ms, optional_ptr<ClientContext> context = nullptr);
	static void SleepMicroSeconds(idx_t micros);
	static thread_id GetThreadId();
	static string GetThreadIdString();
	//! The size of the calling thread's stack and the bytes of it left below the caller. False where the platform does
	//! not report the stack, or where the caller runs outside the stack its thread was first found on, such as on a
	//! fiber's
	static bool GetStackSpace(idx_t &stack_size, idx_t &remaining);
	//! Read the stack limit again where the calling thread's stack follows it, as a Linux main thread's does
	static void RefreshStackLimit();
};

} // namespace duckdb
