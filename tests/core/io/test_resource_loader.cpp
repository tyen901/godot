/**************************************************************************/
/*  test_resource_loader.cpp                                              */
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

TEST_FORCE_LINK(test_resource_loader)

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"

namespace TestResourceLoader {

#ifdef THREADS_ENABLED
class RetirementLoader : public ResourceFormatLoader {
public:
	ObjectID resource_id;
	virtual bool recognize_path(const String &p_path, const String &p_for_type = String()) const override {
		return p_path.get_extension() == "retirementtest";
	}
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) override {
		Ref<Resource> resource;
		resource.instantiate();
		resource_id = resource->get_instance_id();
		if (r_error) {
			*r_error = OK;
		}
		return resource;
	}
};

struct BlockPool {
	Semaphore entered;
	Semaphore release;
};

static void block_pool(void *p_userdata) {
	BlockPool *block = static_cast<BlockPool *>(p_userdata);
	block->entered.post();
	block->release.wait();
}

TEST_CASE("[ResourceLoader] Inline completion retains and retires the queued native callback") {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	BlockPool blocked;
	LocalVector<WorkerThreadPool::TaskID> blockers;
	for (int i = 0; i < pool->get_thread_count(); i++) {
		blockers.push_back(pool->add_native_task(block_pool, &blocked, true));
	}
	for (uint32_t i = 0; i < blockers.size(); i++) {
		blocked.entered.wait();
	}
	Ref<RetirementLoader> loader;
	loader.instantiate();
	ResourceLoader::add_resource_format_loader(loader, true);
	const String path = "res://native_callback.retirementtest";
	Ref<ResourceLoader::LoadToken> token = ResourceLoader::_load_start(path, "", ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_REUSE, true);
	REQUIRE(token.is_valid());
	ObjectID token_id = token->get_instance_id();
	CHECK(ResourceLoader::load_threaded_request(path) == OK);
	Error error = FAILED;
	Ref<Resource> resource = ResourceLoader::_load_complete(*token.ptr(), &error);
	CHECK(error == OK);
	CHECK(resource.is_valid());
	token.unref();
	CHECK(ResourceLoader::load_threaded_get(path) == resource);
	CHECK(ResourceLoader::has_pending_load(path));
	CHECK(ResourceLoader::load_threaded_get(path) == resource);
	CHECK(ResourceLoader::load_threaded_get_status(path) == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE);
	resource.unref();
	// User gets are drained, but the queued callback still owns both objects.
	CHECK(ResourceLoader::has_pending_load("res://./native_callback.retirementtest"));
	CHECK(ObjectDB::get_instance(token_id) != nullptr);
	CHECK(ObjectDB::get_instance(loader->resource_id) != nullptr);
	// An overlapping request reacquires the still-live canonical transaction.
	CHECK(ResourceLoader::load_threaded_request(path) == OK);
	CHECK(ResourceLoader::load_threaded_get(path).is_valid());
	CHECK(ResourceLoader::has_pending_load(path));
	blocked.release.post(blockers.size());
	for (WorkerThreadPool::TaskID blocker : blockers) {
		CHECK(pool->wait_for_task_completion(blocker) == OK);
	}
	uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 5000000;
	while (ResourceLoader::has_pending_load(path) && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK_FALSE(ResourceLoader::has_pending_load(path));
	CHECK(ObjectDB::get_instance(token_id) == nullptr);
	CHECK(ObjectDB::get_instance(loader->resource_id) == nullptr);
	// A fresh request for the same canonical path owns a new transaction.
	token = ResourceLoader::_load_start(path, "", ResourceLoader::LOAD_THREAD_FROM_CURRENT, ResourceLoader::CACHE_MODE_REUSE, true);
	CHECK(token.is_valid());
	token.unref();
	CHECK(ResourceLoader::has_pending_load(path));
	resource = ResourceLoader::load_threaded_get(path);
	CHECK(resource.is_valid());
	resource.unref();
	deadline = OS::get_singleton()->get_ticks_usec() + 5000000;
	while (ResourceLoader::has_pending_load(path) && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK_FALSE(ResourceLoader::has_pending_load(path));
	CHECK(ObjectDB::get_instance(loader->resource_id) == nullptr);
	ResourceLoader::remove_resource_format_loader(loader);
}
class MainDispatchProbe : public Object {
public:
	bool ran = false;
	bool on_main_thread = false;
	Error queued = FAILED;
	void run() {
		ran = true;
		on_main_thread = Thread::is_main_thread();
	}
};

static void queue_main_load_callback(void *p_userdata) {
	MainDispatchProbe *probe = static_cast<MainDispatchProbe *>(p_userdata);
	CallQueue local_queue;
	MessageQueue::set_thread_singleton_override(&local_queue);
	probe->queued = ResourceLoader::call_deferred_on_main_thread(callable_mp(probe, &MainDispatchProbe::run));
	local_queue.flush();
	MessageQueue::set_thread_singleton_override(nullptr);
}

TEST_CASE("[ResourceLoader] Main publication bypasses the worker-local message queue") {
	MainDispatchProbe probe;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	WorkerThreadPool::TaskID task = pool->add_native_task(queue_main_load_callback, &probe, true);
	CHECK(pool->wait_for_task_completion(task) == OK);
	CHECK(probe.queued == OK);
	CHECK_FALSE(probe.ran);
	MessageQueue::get_main_singleton()->flush();
	CHECK(probe.ran);
	CHECK(probe.on_main_thread);
}

class RetainedMainDispatchProbe : public RefCounted {
public:
	bool *ran = nullptr;
	void run() {
		*ran = Thread::is_main_thread();
	}
};

TEST_CASE("[ResourceLoader] Main publication retains its reference-counted callable target") {
	bool ran = false;
	Ref<RetainedMainDispatchProbe> probe;
	probe.instantiate();
	probe->ran = &ran;
	ObjectID owner_id = probe->get_instance_id();
	CHECK(ResourceLoader::call_deferred_on_main_thread(callable_mp(probe.ptr(), &RetainedMainDispatchProbe::run)) == OK);
	probe.unref();
	CHECK(ObjectDB::get_instance(owner_id) != nullptr);
	MessageQueue::get_main_singleton()->flush();
	CHECK(ran);
	CHECK(ObjectDB::get_instance(owner_id) == nullptr);
}

class ConnectionOwnerLoader : public ResourceFormatLoader {
public:
	ObjectID signal_source;
	bool propagated_source_alive = false;
	void changed() {}
	virtual bool recognize_path(const String &p_path, const String &p_for_type = String()) const override {
		return p_path.get_extension() == "connectiontest";
	}
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) override {
		Ref<Resource> resource;
		if (p_path.get_file() == "parent.connectiontest") {
			resource = ResourceLoader::load("res://child.connectiontest");
			propagated_source_alive = ObjectDB::get_instance(signal_source) != nullptr;
		} else {
			Ref<Resource> source;
			source.instantiate();
			signal_source = source->get_instance_id();
			source->connect_changed(callable_mp(this, &ConnectionOwnerLoader::changed));
			// The returned graph intentionally does not own the signal source.
			resource.instantiate();
		}
		if (r_error) {
			*r_error = OK;
		}
		return resource;
	}
};

TEST_CASE("[ResourceLoader] Propagated changed connections retain their source through the outer load") {
	Ref<ConnectionOwnerLoader> loader;
	loader.instantiate();
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<Resource> resource = ResourceLoader::load("res://parent.connectiontest");
	CHECK(resource.is_valid());
	CHECK(loader->propagated_source_alive);
	CHECK(ObjectDB::get_instance(loader->signal_source) == nullptr);
	resource.unref();
	ResourceLoader::remove_resource_format_loader(loader);
}

#endif

} // namespace TestResourceLoader
