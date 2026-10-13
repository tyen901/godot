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
struct RetainedCompletionFixture {
	WorkerThreadPool *pool = nullptr;
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	Semaphore release;
	Semaphore entered;
	SafeNumeric<uint32_t> ran;
};

struct RetainedCompletionWaiter {
	RetainedCompletionFixture *fixture = nullptr;
	Error first = FAILED;
	Error second = FAILED;
};

static void retained_completion_work(void *p_userdata) {
	RetainedCompletionFixture *fixture = static_cast<RetainedCompletionFixture *>(p_userdata);
	fixture->release.wait();
	fixture->ran.increment();
}

static void retained_completion_wait(void *p_userdata) {
	RetainedCompletionWaiter *waiter = static_cast<RetainedCompletionWaiter *>(p_userdata);
	waiter->fixture->entered.post();
	waiter->first = waiter->fixture->pool->wait_for_task_completion_without_reclaim(waiter->fixture->task);
	waiter->second = waiter->fixture->pool->wait_for_task_completion_without_reclaim(waiter->fixture->task);
}

TEST_CASE("[WorkerThreadPool] Retain completion for concurrent and repeated native waits") {
	RetainedCompletionFixture fixture;
	fixture.pool = WorkerThreadPool::get_singleton();
	fixture.task = fixture.pool->add_native_task(retained_completion_work, &fixture, true);
	RetainedCompletionWaiter first;
	RetainedCompletionWaiter second;
	first.fixture = &fixture;
	second.fixture = &fixture;
	Thread first_thread;
	Thread second_thread;
	first_thread.start(retained_completion_wait, &first);
	second_thread.start(retained_completion_wait, &second);
	fixture.entered.wait();
	fixture.entered.wait();
	fixture.release.post();
	first_thread.wait_to_finish();
	second_thread.wait_to_finish();
	CHECK(first.first == OK);
	CHECK(first.second == OK);
	CHECK(second.first == OK);
	CHECK(second.second == OK);
	CHECK(fixture.ran.get() == 1);
	CHECK(fixture.pool->is_task_completed(fixture.task));
	CHECK(fixture.pool->wait_for_task_completion(fixture.task) == OK);
	ERR_PRINT_OFF;
	CHECK(fixture.pool->wait_for_task_completion_without_reclaim(fixture.task) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

struct CollaborativeCompletionFixture {
	WorkerThreadPool *pool = nullptr;
	SafeNumeric<uint32_t> ran;
	Error first = FAILED;
	Error second = FAILED;
	Error reclaimed = FAILED;
};

static void collaborative_completion_child(void *p_userdata) {
	static_cast<CollaborativeCompletionFixture *>(p_userdata)->ran.increment();
}

static void collaborative_completion_parent(void *p_userdata) {
	CollaborativeCompletionFixture *fixture = static_cast<CollaborativeCompletionFixture *>(p_userdata);
	WorkerThreadPool::TaskID child = fixture->pool->add_native_task(collaborative_completion_child, fixture, true);
	fixture->first = fixture->pool->wait_for_task_completion_without_reclaim(child);
	fixture->second = fixture->pool->wait_for_task_completion_without_reclaim(child);
	fixture->reclaimed = fixture->pool->wait_for_task_completion(child);
}

TEST_CASE("[WorkerThreadPool] Retained native completion preserves one-worker collaborative progress") {
	WorkerThreadPool pool(false);
	pool.init(1);
	CollaborativeCompletionFixture fixture;
	fixture.pool = &pool;
	WorkerThreadPool::TaskID parent = pool.add_native_task(collaborative_completion_parent, &fixture, true);
	CHECK(pool.wait_for_task_completion(parent) == OK);
	CHECK(fixture.first == OK);
	CHECK(fixture.second == OK);
	CHECK(fixture.reclaimed == OK);
	CHECK(fixture.ran.get() == 1);
}

struct NativeCompletionSaturation {
	WorkerThreadPool *pool = nullptr;
	Semaphore entered;
	Semaphore release;
	SafeNumeric<uint32_t> compiled;
	SafeNumeric<uint32_t> joined;
	SafeFlag start_failed;
};

struct NativeCompiler {
	NativeCompletionSaturation *fixture = nullptr;
	WorkerThreadPool::NativeCompletion *completion = nullptr;
};

static void native_compiler_task(void *p_userdata) {
	static_cast<NativeCompletionSaturation *>(p_userdata)->compiled.increment();
}

static void native_compiler_thread(void *p_userdata) {
	NativeCompiler *compiler = static_cast<NativeCompiler *>(p_userdata);
	WorkerThreadPool *pool = compiler->fixture->pool;
	WorkerThreadPool::TaskID task = pool->add_native_task(native_compiler_task, compiler->fixture, true);
	pool->wait_for_task_completion(task);
	pool->complete_native_completion(*compiler->completion);
}

static void saturated_native_completion_caller(void *p_userdata) {
	NativeCompletionSaturation *fixture = static_cast<NativeCompletionSaturation *>(p_userdata);
	fixture->entered.post();
	fixture->release.wait();
	WorkerThreadPool::NativeCompletion completion;
	NativeCompiler compiler;
	compiler.fixture = fixture;
	compiler.completion = &completion;
	Thread worker;
	worker.start(native_compiler_thread, &compiler);
	if (!worker.is_started()) {
		fixture->start_failed.set();
		return;
	}
	fixture->pool->wait_for_native_completion(completion, true);
	worker.wait_to_finish();
	fixture->joined.increment();
}

TEST_CASE("[WorkerThreadPool] Native completion donates a fully saturated high-priority pool") {
	WorkerThreadPool pool(false);
	pool.init(2);
	NativeCompletionSaturation fixture;
	fixture.pool = &pool;
	WorkerThreadPool::TaskID first = pool.add_native_task(saturated_native_completion_caller, &fixture, true);
	WorkerThreadPool::TaskID second = pool.add_native_task(saturated_native_completion_caller, &fixture, true);
	// Every worker is occupied before either native compiler may queue its work.
	fixture.entered.wait();
	fixture.entered.wait();
	fixture.release.post(2);
	CHECK(pool.wait_for_task_completion(first) == OK);
	CHECK(pool.wait_for_task_completion(second) == OK);
	CHECK_FALSE(fixture.start_failed.is_set());
	CHECK(fixture.compiled.get() == 2);
	CHECK(fixture.joined.get() == 2);
}

static void saturated_native_completion_group_caller(void *p_userdata, uint32_t p_index) {
	saturated_native_completion_caller(p_userdata);
}

TEST_CASE("[WorkerThreadPool] Native completion donates saturated high-priority group callers") {
	WorkerThreadPool pool(false);
	pool.init(2);
	NativeCompletionSaturation fixture;
	fixture.pool = &pool;
	WorkerThreadPool::GroupID group = pool.add_native_group_task(saturated_native_completion_group_caller, &fixture, 2, 2, true);
	fixture.entered.wait();
	fixture.entered.wait();
	fixture.release.post(2);
	pool.wait_for_group_task_completion(group);
	CHECK_FALSE(fixture.start_failed.is_set());
	CHECK(fixture.compiled.get() == 2);
	CHECK(fixture.joined.get() == 2);
}

struct NestedNativeCompletion {
	WorkerThreadPool *pool = nullptr;
	WorkerThreadPool::NativeCompletion outer;
	WorkerThreadPool::NativeCompletion inner;
	Semaphore outer_entered;
	Semaphore inner_entered;
	SafeFlag inner_returned;
	SafeFlag probe_saw_pending_inner;
	SafeFlag outer_returned;
};

static void nested_native_completion_inner(void *p_userdata) {
	NestedNativeCompletion *fixture = static_cast<NestedNativeCompletion *>(p_userdata);
	fixture->inner_entered.post();
	fixture->pool->wait_for_native_completion(fixture->inner, true);
	fixture->inner_returned.set();
}

static void nested_native_completion_outer(void *p_userdata) {
	NestedNativeCompletion *fixture = static_cast<NestedNativeCompletion *>(p_userdata);
	fixture->outer_entered.post();
	fixture->pool->wait_for_native_completion(fixture->outer, true);
	fixture->outer_returned.set();
}

static void nested_native_completion_probe(void *p_userdata) {
	NestedNativeCompletion *fixture = static_cast<NestedNativeCompletion *>(p_userdata);
	fixture->probe_saw_pending_inner.set_to(!fixture->inner_returned.is_set());
	fixture->pool->complete_native_completion(fixture->inner);
}

TEST_CASE("[WorkerThreadPool] Native completion keeps exact ownership across forced nested donation") {
	WorkerThreadPool pool(false);
	pool.init(1);
	NestedNativeCompletion fixture;
	fixture.pool = &pool;
	WorkerThreadPool::TaskID outer = pool.add_native_task(nested_native_completion_outer, &fixture, true);
	fixture.outer_entered.wait();
	WorkerThreadPool::TaskID inner = pool.add_native_task(nested_native_completion_inner, &fixture, true);
	fixture.inner_entered.wait();
	// The only worker is now donating inside the inner wait. Completing the
	// outer record must neither release the inner wait nor lose outer completion.
	pool.complete_native_completion(fixture.outer);
	WorkerThreadPool::TaskID probe = pool.add_native_task(nested_native_completion_probe, &fixture, true);
	CHECK(pool.wait_for_task_completion(outer) == OK);
	CHECK(pool.wait_for_task_completion(inner) == OK);
	CHECK(pool.wait_for_task_completion(probe) == OK);
	CHECK(fixture.probe_saw_pending_inner.is_set());
	CHECK(fixture.inner_returned.is_set());
	CHECK(fixture.outer_returned.is_set());
}

struct NativeCompletionPumpSkip {
	WorkerThreadPool *pool = nullptr;
	WorkerThreadPool::NativeCompletion completion;
	Semaphore caller_entered;
	Semaphore blocker_entered;
	Semaphore release_blocker;
	Semaphore nested_entered;
	Semaphore release_nested;
	SafeFlag pump_ran;
	SafeFlag dependency_skipped_pump;

	void pump() {
		pump_ran.set();
	}
};

static void native_completion_pump_caller(void *p_userdata) {
	NativeCompletionPumpSkip *fixture = static_cast<NativeCompletionPumpSkip *>(p_userdata);
	fixture->caller_entered.post();
	fixture->pool->wait_for_native_completion(fixture->completion, true);
}

static void native_completion_pump_blocker(void *p_userdata) {
	NativeCompletionPumpSkip *fixture = static_cast<NativeCompletionPumpSkip *>(p_userdata);
	fixture->blocker_entered.post();
	fixture->release_blocker.wait();
}

static void native_completion_pump_dependency(void *p_userdata) {
	NativeCompletionPumpSkip *fixture = static_cast<NativeCompletionPumpSkip *>(p_userdata);
	fixture->dependency_skipped_pump.set_to(!fixture->pump_ran.is_set());
	fixture->pool->complete_native_completion(fixture->completion);
}

static void native_completion_pump_nested(void *p_userdata) {
	NativeCompletionPumpSkip *fixture = static_cast<NativeCompletionPumpSkip *>(p_userdata);
	fixture->nested_entered.post();
	fixture->release_nested.wait();
	WorkerThreadPool::TaskID dependency = fixture->pool->add_native_task(native_completion_pump_dependency, fixture, true);
	// The enclosing native wait's pump exclusion must survive this default wait.
	fixture->pool->wait_for_task_completion(dependency);
}

TEST_CASE("[WorkerThreadPool] Native completion can reach work queued behind an excluded pump task") {
	WorkerThreadPool pool(false);
	pool.init(2);
	NativeCompletionPumpSkip fixture;
	fixture.pool = &pool;
	WorkerThreadPool::TaskID blocker = pool.add_native_task(native_completion_pump_blocker, &fixture, true);
	fixture.blocker_entered.wait();
	WorkerThreadPool::TaskID caller = pool.add_native_task(native_completion_pump_caller, &fixture, true);
	fixture.caller_entered.wait();
	WorkerThreadPool::TaskID pump = pool.add_task(callable_mp(&fixture, &NativeCompletionPumpSkip::pump), true, String(), true);
	WorkerThreadPool::TaskID dependency = pool.add_native_task(native_completion_pump_dependency, &fixture, true);
	CHECK(pool.wait_for_task_completion(caller) == OK);
	fixture.release_blocker.post();
	CHECK(pool.wait_for_task_completion(blocker) == OK);
	CHECK(pool.wait_for_task_completion(dependency) == OK);
	CHECK(pool.wait_for_task_completion(pump) == OK);
	CHECK(fixture.dependency_skipped_pump.is_set());
	CHECK(fixture.pump_ran.is_set());
}

TEST_CASE("[WorkerThreadPool] Native completion inherits pump exclusion through nested ordinary waits") {
	WorkerThreadPool pool(false);
	pool.init(2);
	NativeCompletionPumpSkip fixture;
	fixture.pool = &pool;
	WorkerThreadPool::TaskID blocker = pool.add_native_task(native_completion_pump_blocker, &fixture, true);
	fixture.blocker_entered.wait();
	WorkerThreadPool::TaskID caller = pool.add_native_task(native_completion_pump_caller, &fixture, true);
	fixture.caller_entered.wait();
	WorkerThreadPool::TaskID nested = pool.add_native_task(native_completion_pump_nested, &fixture, true);
	fixture.nested_entered.wait();
	WorkerThreadPool::TaskID pump = pool.add_task(callable_mp(&fixture, &NativeCompletionPumpSkip::pump), true, String(), true);
	fixture.release_nested.post();
	CHECK(pool.wait_for_task_completion(caller) == OK);
	fixture.release_blocker.post();
	CHECK(pool.wait_for_task_completion(blocker) == OK);
	CHECK(pool.wait_for_task_completion(nested) == OK);
	CHECK(pool.wait_for_task_completion(pump) == OK);
	CHECK(fixture.dependency_skipped_pump.is_set());
	CHECK(fixture.pump_ran.is_set());
}

#endif

} // namespace TestWorkerThreadPool
