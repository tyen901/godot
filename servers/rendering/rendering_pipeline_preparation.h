/**************************************************************************/
/*  rendering_pipeline_preparation.h                                      */
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

#include "core/io/resource.h"
#include "core/os/condition_variable.h"

// A render-thread snapshot pins native shader state until worker preparation has
// finished. Mutation and shutdown join these pins before releasing native data.
class PipelinePreparationTracker {
	BinaryMutex mutex;
	ConditionVariable condition;
	uint32_t active = 0;

public:
	void begin() {
		MutexLock lock(mutex);
		active++;
	}
	void end() {
		MutexLock lock(mutex);
		DEV_ASSERT(active > 0);
		active--;
		if (active == 0) {
			condition.notify_all();
		}
	}
	void wait() {
		MutexLock lock(mutex);
		while (active > 0) {
			condition.wait(lock);
		}
	}
};

class RenderingPipelinePreparation : public RefCounted {
	inline static PipelinePreparationTracker preparations;
	Ref<Resource> base;
	Ref<Resource> material_override;

public:
	Error error = OK;
	void retain_resources(const Ref<Resource> &p_base, const Ref<Resource> &p_material_override) {
		base = p_base;
		material_override = p_material_override;
	}
	virtual Error compile() = 0;
	static void wait_for_all() { preparations.wait(); }
	RenderingPipelinePreparation() { preparations.begin(); }
	virtual ~RenderingPipelinePreparation() {
		material_override.unref();
		base.unref();
		preparations.end();
	}
};
