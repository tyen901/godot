/**************************************************************************/
/*  test_worker_thread_pool.cpp                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_worker_thread_pool)

#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"

struct WorkerThreadPoolTestAccess {
	static uint32_t user_waiters(WorkerThreadPool &p_pool, WorkerThreadPool::TaskID p_task) {
		MutexLock lock(p_pool.task_mutex);
		auto task = p_pool.tasks.getptr(p_task);
		return task ? (*task)->waiting_user : 0;
	}
};

namespace TestWorkerThreadPool {

static LocalVector<SafeNumeric<int>> counter;
static SafeFlag exit;

static void static_test(void *p_arg) {
	counter[(uint64_t)p_arg].increment();
	counter[0].add(2);
}
static void static_callable_test() {
	counter[0].sub(2);
}
TEST_CASE("[WorkerThreadPool] Process threads using individual tasks") {
	for (int iterations = 0; iterations < 500; iterations++) {
		const int count = Math::pow(2.0f, Math::random(0.0f, 5.0f));
		const bool low_priority = Math::rand() % 2;

		LocalVector<WorkerThreadPool::TaskID> tasks1;
		LocalVector<WorkerThreadPool::TaskID> tasks2;
		tasks1.resize(count);
		tasks2.resize(count);

		counter.clear();
		counter.resize(count);
		for (int i = 0; i < count; i++) {
			tasks1[i] = WorkerThreadPool::get_singleton()->add_native_task(static_test, (void *)(uintptr_t)i, low_priority);
			tasks2[i] = WorkerThreadPool::get_singleton()->add_task(callable_mp_static(static_callable_test), !low_priority);
		}
		for (int i = 0; i < count; i++) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(tasks1[i]);
			WorkerThreadPool::get_singleton()->wait_for_task_completion(tasks2[i]);
		}

		bool all_run_once = true;
		for (int i = 0; i < count; i++) {
			//Reduce number of check messages
			all_run_once &= counter[i].get() == 1;
		}
		CHECK(all_run_once);
	}
}

static void static_group_test(void *p_arg, uint32_t p_index) {
	counter[p_index].increment();
	counter[0].add((uintptr_t)p_arg);
}
static void static_callable_group_test(uint32_t p_index) {
	counter[p_index].increment();
	counter[0].sub(2);
}
TEST_CASE("[WorkerThreadPool] Process elements using group tasks") {
	for (int iterations = 0; iterations < 500; iterations++) {
		const int count = Math::pow(2.0f, Math::random(0.0f, 5.0f));
		const int tasks = Math::pow(2.0f, Math::random(0.0f, 5.0f));
		const bool low_priority = Math::rand() % 2;

		counter.clear();
		counter.resize(count);
		WorkerThreadPool::GroupID group1 = WorkerThreadPool::get_singleton()->add_native_group_task(static_group_test, (void *)2, count, tasks, !low_priority);
		WorkerThreadPool::GroupID group2 = WorkerThreadPool::get_singleton()->add_group_task(callable_mp_static(static_callable_group_test), count, tasks, low_priority);
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group1);
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group2);

		bool all_run_once = true;
		for (int i = 0; i < count; i++) {
			//Reduce number of check messages
			all_run_once &= counter[i].get() == 2;
		}
		CHECK(all_run_once);
	}
}

static void static_test_daemon(void *p_arg) {
	while (!exit.is_set()) {
		counter[0].add(1);
		WorkerThreadPool::get_singleton()->yield();
	}
}

static void static_busy_task(void *p_arg) {
	while (!exit.is_set()) {
		OS::get_singleton()->delay_usec(1);
	}
}

static void static_legit_task(void *p_arg) {
	*((bool *)p_arg) = counter[0].get() > 0;
	counter[1].add(1);
}

TEST_CASE("[WorkerThreadPool] Run a yielding daemon as the only hope for other tasks to run") {
	exit.clear();
	counter.clear();
	counter.resize(2);

	WorkerThreadPool::TaskID daemon_task_id = WorkerThreadPool::get_singleton()->add_native_task(static_test_daemon, nullptr, true);

	int num_threads = WorkerThreadPool::get_singleton()->get_thread_count();

	// Keep all the other threads busy.
	LocalVector<WorkerThreadPool::TaskID> task_ids;
	for (int i = 0; i < num_threads - 1; i++) {
		task_ids.push_back(WorkerThreadPool::get_singleton()->add_native_task(static_busy_task, nullptr, true));
	}

	LocalVector<bool> legit_task_needed_yield;
	int legit_tasks_count = num_threads * 4;
	legit_task_needed_yield.resize(legit_tasks_count);
	for (int i = 0; i < legit_tasks_count; i++) {
		legit_task_needed_yield[i] = false;
		task_ids.push_back(WorkerThreadPool::get_singleton()->add_native_task(static_legit_task, &legit_task_needed_yield[i], i >= legit_tasks_count / 2));
	}

	while (counter[1].get() != legit_tasks_count) {
		OS::get_singleton()->delay_usec(1);
	}

	exit.set();
	for (uint32_t i = 0; i < task_ids.size(); i++) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_ids[i]);
	}
	WorkerThreadPool::get_singleton()->notify_yield_over(daemon_task_id);
	WorkerThreadPool::get_singleton()->wait_for_task_completion(daemon_task_id);

	CHECK_MESSAGE(counter[0].get() > 0, "Daemon task should have looped at least once.");
	CHECK_MESSAGE(counter[1].get() == legit_tasks_count, "All legit tasks should have been able to run.");

	bool all_needed_yield = true;
	for (int i = 0; i < legit_tasks_count; i++) {
		if (!legit_task_needed_yield[i]) {
			all_needed_yield = false;
			break;
		}
	}
	CHECK_MESSAGE(all_needed_yield, "All legit tasks should have needed the daemon yielding to run.");
}

#ifdef THREADS_ENABLED
struct NativeTaskRetirement {
	WorkerThreadPool *pool = nullptr;
	Semaphore entered;
	Semaphore finish;
	SafeFlag callback_finished;
	SafeFlag parent_finished;
	WorkerThreadPool::TaskID child = WorkerThreadPool::INVALID_TASK_ID;
	Error waited = FAILED;
};

static void retire_current_native_task(void *p_userdata) {
	NativeTaskRetirement *fixture = static_cast<NativeTaskRetirement *>(p_userdata);
	fixture->pool->release_task(fixture->pool->get_caller_task_id());
	fixture->entered.post();
	fixture->finish.wait();
	fixture->callback_finished.set();
}

static void wait_for_retiring_native_task(void *p_userdata) {
	NativeTaskRetirement *fixture = static_cast<NativeTaskRetirement *>(p_userdata);
	fixture->child = fixture->pool->add_native_task(retire_current_native_task, fixture, true);
	fixture->waited = fixture->pool->wait_for_task_completion(fixture->child);
	fixture->parent_finished.set();
}

TEST_CASE("[WorkerThreadPool] Native retirement keeps its callback and registered waiter alive") {
	WorkerThreadPool pool(false);
	pool.init(1);
	NativeTaskRetirement fixture;
	fixture.pool = &pool;
	// The only worker enters a collaborative wait before running the child.
	WorkerThreadPool::TaskID parent = pool.add_native_task(wait_for_retiring_native_task, &fixture, true);
	fixture.entered.wait();
	CHECK_FALSE(pool.is_task_completed(fixture.child));
	CHECK_FALSE(fixture.parent_finished.is_set());
	fixture.finish.post();
	CHECK(pool.wait_for_task_completion(parent) == OK);
	CHECK(fixture.callback_finished.is_set());
	CHECK(fixture.parent_finished.is_set());
	CHECK(fixture.waited == OK);
	ERR_PRINT_OFF;
	CHECK(pool.wait_for_task_completion(fixture.child) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

static void completed_native_retirement_work(void *p_userdata) {
	static_cast<SafeNumeric<uint32_t> *>(p_userdata)->increment();
}

TEST_CASE("[WorkerThreadPool] Native retirement also releases an already completed task") {
	WorkerThreadPool pool(false);
	pool.init(1);
	SafeNumeric<uint32_t> ran;
	WorkerThreadPool::TaskID task = pool.add_native_task(completed_native_retirement_work, &ran, true);
	uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 5000000;
	while (!pool.is_task_completed(task) && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(pool.is_task_completed(task));
	CHECK(ran.get() == 1);
	pool.release_task(task);
	ERR_PRINT_OFF;
	CHECK(pool.wait_for_task_completion(task) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

struct UserRetirementWaiter {
	WorkerThreadPool *pool = nullptr;
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	Semaphore finish;
	Error result = FAILED;
};

static void user_retirement_work(void *p_userdata) {
	static_cast<UserRetirementWaiter *>(p_userdata)->finish.wait();
}

static void user_retirement_wait(void *p_userdata) {
	UserRetirementWaiter *fixture = static_cast<UserRetirementWaiter *>(p_userdata);
	fixture->result = fixture->pool->wait_for_task_completion(fixture->task);
}

TEST_CASE("[WorkerThreadPool] Native retirement preserves an already registered user waiter") {
	WorkerThreadPool pool(false);
	pool.init(1);
	UserRetirementWaiter fixture;
	fixture.pool = &pool;
	fixture.task = pool.add_native_task(user_retirement_work, &fixture, true);
	Thread waiter;
	waiter.start(user_retirement_wait, &fixture);
	uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 5000000;
	while (WorkerThreadPoolTestAccess::user_waiters(pool, fixture.task) == 0 && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(WorkerThreadPoolTestAccess::user_waiters(pool, fixture.task) == 1);
	pool.release_task(fixture.task);
	CHECK_FALSE(pool.is_task_completed(fixture.task));
	fixture.finish.post();
	waiter.wait_to_finish();
	CHECK(fixture.result == OK);
	ERR_PRINT_OFF;
	CHECK(pool.wait_for_task_completion(fixture.task) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

#endif

} // namespace TestWorkerThreadPool
