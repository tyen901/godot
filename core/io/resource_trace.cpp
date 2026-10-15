/**************************************************************************/
/*  resource_trace.cpp                                                         */
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

#include "resource_trace.h"

#include "core/crypto/crypto_core.h"
#include "core/os/os.h"
#include "core/os/thread.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ResourceTrace {
namespace {
struct Output {
	std::mutex mutex;
	FILE *file = nullptr;
	uint64_t sequence = 0;
	uint64_t limit = 200000;
	bool attempted = false;
	bool stopped = false;
};
Output &output() {
	static Output value;
	return value;
}
thread_local uint64_t current_operation = 0;
uint64_t native_thread_id() {
#ifdef __linux__
	return uint64_t(syscall(SYS_gettid));
#else
	return 0; // Native OS thread correlation is currently implemented for Linux.
#endif
}
uint64_t write(const char *p_phase, const char *p_stage, uint64_t p_operation, uint64_t p_parent, const String &p_fields) {
	Output &out = output();
	std::lock_guard<std::mutex> lock(out.mutex);
	if (out.stopped) {
		return 0;
	}
	if (!out.attempted) {
		out.attempted = true;
		out.file = std::fopen(std::getenv("GODOT_RESOURCE_TRACE"), "wx");
		const char *limit = std::getenv("GODOT_RESOURCE_TRACE_LIMIT");
		if (limit) {
			char *end = nullptr;
			uint64_t parsed = std::strtoull(limit, &end, 10);
			if (*limit && end && !*end && parsed > 0 && parsed <= 2000000) {
				out.limit = parsed;
			}
		}
		if (!out.file) {
			std::fprintf(stderr, "Resource trace: cannot create output file.\n");
			out.stopped = true;
			return 0;
		}
	}
	if (out.sequence >= out.limit) {
		std::fprintf(out.file, "{\"schema\":\"godot-resource-trace-v1\",\"phase\":\"truncated\",\"sequence\":%llu}\n", (unsigned long long)++out.sequence);
		std::fflush(out.file);
		out.stopped = true;
		return 0;
	}
	if (out.sequence == 0) {
		std::fprintf(out.file, "{\"schema\":\"godot-resource-trace-v1\",\"phase\":\"anchor\",\"sequence\":0,\"ticks_usec\":%llu,\"unix_usec\":%llu,\"record_limit\":%llu}\n", (unsigned long long)OS::get_singleton()->get_ticks_usec(), (unsigned long long)(OS::get_singleton()->get_unix_time() * 1000000.0), (unsigned long long)out.limit);
	}
	uint64_t sequence = ++out.sequence;
	uint64_t operation = p_operation ? p_operation : sequence;
	String row = "{\"schema\":\"godot-resource-trace-v1\",\"sequence\":" + uitos(sequence) +
			",\"ticks_usec\":" + uitos(OS::get_singleton()->get_ticks_usec()) +
			",\"engine_thread_id\":\"" + uitos(Thread::get_caller_id()) + "\",\"os_thread_id\":\"" + uitos(native_thread_id()) + "\",\"phase\":\"" + p_phase +
			"\",\"stage\":\"" + p_stage + "\",\"operation\":" + uitos(operation) +
			",\"parent_operation\":" + uitos(p_parent) + p_fields + "}\n";
	CharString bytes = row.utf8();
	if (std::fwrite(bytes.get_data(), 1, bytes.length(), out.file) != size_t(bytes.length()) || std::fflush(out.file) != 0) {
		std::fprintf(stderr, "Resource trace: output write failed.\n");
		out.stopped = true;
		return 0;
	}
	return operation;
}
} //namespace
bool enabled() {
	static const bool value = [] {
		const char *path = std::getenv("GODOT_RESOURCE_TRACE");
		return path && *path;
	}();
	return value;
}
String field(const char *p_key, const String &p_value) {
	return String(",\"") + p_key + "\":\"" + p_value.json_escape() + "\"";
}
String bytes_fields(const char *p_domain, const uint8_t *p_data, size_t p_size) {
	unsigned char digest[32];
	Error error = CryptoCore::sha256(p_data, p_size, digest);
	String fields = field("hash_domain", p_domain) + field("hash_bytes", uitos(p_size));
	if (error != OK) {
		return fields + field("sha256_error", itos(error));
	}
	return fields + field("sha256", String::hex_encode_buffer(digest, sizeof(digest)));
}
String descriptor_fields(const char *p_domain, const String &p_descriptor) {
	CharString bytes = p_descriptor.utf8();
	return field("descriptor", p_descriptor) + bytes_fields(p_domain, (const uint8_t *)bytes.get_data(), bytes.length());
}
void event(const char *p_stage, const String &p_fields) {
	write("event", p_stage, 0, current_operation, p_fields);
}
Scope::Scope(const char *p_stage, const String &p_fields) : stage(p_stage) {
	if (enabled()) {
		parent = current_operation;
		operation = write("begin", stage, 0, parent, p_fields);
		if (operation) {
			current_operation = operation;
		}
	}
}
void Scope::finish(const String &p_fields) {
	if (operation) {
		write("end", stage, operation, parent, p_fields);
		current_operation = parent;
		operation = 0;
	}
}
Scope::~Scope() {
	finish(); // A returned scope is not a success claim. Explicit results are separate fields.
}
} //namespace ResourceTrace
