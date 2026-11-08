#include "duckdb/common/thread.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/original/std/sstream.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/time_point.hpp"
#include "duckdb/main/client_context.hpp"

#if defined(_WIN32)
#include "duckdb/common/windows.hpp"
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#elif !defined(DUCKDB_NO_THREADS) && (defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__))
#define DUCKDB_THREAD_UTIL_PTHREAD_STACK
#include <pthread.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif
#if defined(__linux__)
#include <cstring>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace duckdb {

#ifndef DUCKDB_NO_THREADS
void ThreadUtil::SleepMs(idx_t sleep_ms, optional_ptr<ClientContext> context) {
	auto start_time = TimePoint::Tick();
	static constexpr idx_t DEFAULT_SLEEP_INTERVAL_MS = 100;

	while (true) {
		if (context && context->IsInterrupted()) {
			throw InterruptException();
		}
		auto elapsed_ms = static_cast<idx_t>(start_time.ElapsedMillis());
		if (elapsed_ms >= sleep_ms) {
			break;
		}
		auto remaining_ms = sleep_ms - elapsed_ms;
		std::this_thread::sleep_for(milliseconds(MinValue(remaining_ms, DEFAULT_SLEEP_INTERVAL_MS)));
	}
}

void ThreadUtil::SleepMicroSeconds(idx_t micros) {
	std::this_thread::sleep_for(microseconds(micros));
}

thread_id ThreadUtil::GetThreadId() {
	return std::this_thread::get_id();
}

string ThreadUtil::GetThreadIdString() {
	std::ostringstream ss;
	ss << std::this_thread::get_id();
	return ss.str();
}

#else

void ThreadUtil::SleepMs(idx_t sleep_ms, optional_ptr<ClientContext>) {
	throw InvalidInputException("ThreadUtil::SleepMs requires DuckDB to be compiled with thread support");
}

void ThreadUtil::SleepMicroSeconds(idx_t micros) {
	throw InvalidInputException("ThreadUtil::SleepMicroSeconds requires DuckDB to be compiled with thread support");
}

thread_id ThreadUtil::GetThreadId() {
	return 0;
}

string ThreadUtil::GetThreadIdString() {
	return "0";
}

#endif

namespace {

struct ThreadStackBounds {
	bool queried;
	bool known;
	//! Whether the low end lies the soft stack limit below the top, as for the main thread of a Linux process
	bool follows_limit;
	uintptr_t low;
	uintptr_t high;
	uintptr_t top;
};

ThreadStackBounds &GetThreadStackBounds() {
	// trivially destructible, so its first use in a thread registers no destructor that could fail to allocate
	static thread_local ThreadStackBounds thread_stack_bounds;
	return thread_stack_bounds;
}

uintptr_t CurrentStackAddress() {
#if defined(_MSC_VER)
	return reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
#else
	// not the address of a local, which AddressSanitizer may move to a fake stack on the heap
	return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
#endif
}

#if defined(DUCKDB_THREAD_UTIL_PTHREAD_STACK) && defined(__linux__)
// The initial stack of a process grows on demand up to the soft stack limit, which can change while the process runs,
// counted from its top, where the kernel puts the executable's name above the arguments and the environment
bool FindInitialStackTop(uintptr_t high, uintptr_t &top) {
	// only the main thread starts on it; a child forked from another thread runs on that thread's stack, far below
	if (getpid() != static_cast<pid_t>(syscall(SYS_gettid))) {
		return false;
	}
	// other stacks are mapped at least 128 MiB below it
	static constexpr uintptr_t MAX_DISTANCE = uintptr_t(64) * 1024 * 1024;
	auto name = reinterpret_cast<const char *>(getauxval(AT_EXECFN));
	if (!name) {
		return false;
	}
	auto name_end = reinterpret_cast<uintptr_t>(name) + strlen(name) + 1;
	// glibc and bionic report the end of the initial stack, just above the name, musl a point below the arguments
	if (name_end <= high ? high - name_end > MAX_DISTANCE : name_end - high > MAX_DISTANCE) {
		return false;
	}
	top = MaxValue(high, name_end);
	return true;
}

bool ApplyStackLimit(ThreadStackBounds &bounds) {
	struct rlimit limit;
	if (getrlimit(RLIMIT_STACK, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= bounds.top) {
		return false;
	}
	bounds.low = bounds.top - limit.rlim_cur;
	return bounds.low < bounds.high;
}
#endif

bool QueryStackBounds(ThreadStackBounds &bounds) {
#if defined(_WIN32)
	MEMORY_BASIC_INFORMATION info;
	if (VirtualQuery(reinterpret_cast<void *>(CurrentStackAddress()), &info, sizeof(info)) == 0) {
		return false;
	}
	// the reservation starts at the lowest address the stack can grow to, its committed pages end at the top
	bounds.low = reinterpret_cast<uintptr_t>(info.AllocationBase);
	bounds.high = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
	return true;
#elif defined(DUCKDB_THREAD_UTIL_PTHREAD_STACK) && defined(__APPLE__)
	auto thread = pthread_self();
	bounds.high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(thread));
	bounds.low = bounds.high - pthread_get_stacksize_np(thread);
	return true;
#elif defined(DUCKDB_THREAD_UTIL_PTHREAD_STACK)
	pthread_attr_t attributes;
#if defined(__FreeBSD__)
	if (pthread_attr_init(&attributes) != 0) {
		return false;
	}
	if (pthread_attr_get_np(pthread_self(), &attributes) != 0) {
		pthread_attr_destroy(&attributes);
		return false;
	}
#else
	if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
		return false;
	}
#endif
	void *address = nullptr;
	size_t size = 0;
	auto result = pthread_attr_getstack(&attributes, &address, &size);
	pthread_attr_destroy(&attributes);
	if (result != 0) {
		return false;
	}
	bounds.low = reinterpret_cast<uintptr_t>(address);
	bounds.high = bounds.low + size;
#if defined(__linux__)
	if (FindInitialStackTop(bounds.high, bounds.top)) {
		// musl reports only the part of the initial stack mapped so far
		bounds.follows_limit = true;
		return ApplyStackLimit(bounds);
	}
#endif
	return true;
#else
	return false;
#endif
}

} // namespace

bool ThreadUtil::GetStackSpace(idx_t &stack_size, idx_t &remaining) {
	auto &bounds = GetThreadStackBounds();
	if (!bounds.queried) {
		bounds.known = QueryStackBounds(bounds);
		bounds.queried = true;
	}
	auto current = CurrentStackAddress();
	if (!bounds.known || current <= bounds.low || current > bounds.high) {
		return false;
	}
	stack_size = bounds.high - bounds.low;
	remaining = current - bounds.low;
	return true;
}

void ThreadUtil::RefreshStackLimit() {
#if defined(DUCKDB_THREAD_UTIL_PTHREAD_STACK) && defined(__linux__)
	auto &bounds = GetThreadStackBounds();
	if (bounds.queried && bounds.follows_limit) {
		bounds.known = ApplyStackLimit(bounds);
	}
#endif
}

#undef DUCKDB_THREAD_UTIL_PTHREAD_STACK

} // namespace duckdb
