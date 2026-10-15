/**************************************************************************/
/*  resource_trace.h                                                         */
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

#pragma once

#include "core/string/ustring.h"

// Opt-in diagnostic output only. No resource references, workers or render policy.
namespace ResourceTrace {
bool enabled();
String field(const char *p_key, const String &p_value);
void event(const char *p_stage, const String &p_fields = String());
class Scope {
	uint64_t operation = 0;
	uint64_t parent = 0;
	const char *stage;

public:
	Scope(const char *p_stage, const String &p_fields);
	~Scope();
	void finish(const String &p_fields = String());
	Scope(const Scope &) = delete;
	Scope &operator=(const Scope &) = delete;
};
} //namespace ResourceTrace
#define RESOURCE_TRACE_SCOPE(variable, stage, fields) ResourceTrace::Scope variable(stage, ResourceTrace::enabled() ? (fields) : String())
#define RESOURCE_TRACE_EVENT(stage, fields) \
	do { \
		if (ResourceTrace::enabled()) { \
			ResourceTrace::event(stage, fields); \
		} \
	} while (false)
