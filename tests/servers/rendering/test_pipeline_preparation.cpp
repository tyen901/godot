/**************************************************************************/
/*  test_pipeline_preparation.cpp                                         */
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

TEST_FORCE_LINK(test_pipeline_preparation)

#include "servers/rendering/storage/render_scene_buffers.h"

namespace TestPipelinePreparation {

using Preparation = RenderSceneBuffers::PipelinePreparation;

TEST_CASE("[Rendering][PipelinePreparation] A request is not a completed draw") {
	Preparation preparation;
	CHECK(preparation.state == Preparation::NONE);
	preparation.request();
	CHECK(preparation.request_id == 1);
	CHECK(preparation.state == Preparation::PENDING);
	preparation.finish();
	CHECK(preparation.state == Preparation::UNSUPPORTED);
	preparation.request();
	preparation.renderer_supported = true;
	preparation.finish();
	CHECK(preparation.state == Preparation::FAILED);
}

TEST_CASE("[Rendering][PipelinePreparation] Valid pipelines require a completed draw") {
	Preparation preparation;
	preparation.request();
	preparation.renderer_supported = true;
	preparation.pipeline_binds = 2;
	preparation.draw_calls = 3;
	preparation.finish();
	CHECK(preparation.state == Preparation::FAILED);
	preparation.request();
	preparation.renderer_supported = true;
	preparation.pipeline_binds = 2;
	preparation.draw_calls = 3;
	preparation.render_completed = true;
	preparation.failed_pipelines = 1;
	preparation.finish();
	CHECK(preparation.state == Preparation::FAILED);
	preparation.request();
	preparation.renderer_supported = true;
	preparation.pipeline_binds = 2;
	preparation.draw_calls = 3;
	preparation.render_completed = true;
	preparation.pipeline_policy = Preparation::POLICY_UBERSHADER_ONLY;
	preparation.finish();
	CHECK(preparation.state == Preparation::DRAWN);
	CHECK(preparation.pipeline_policy == Preparation::POLICY_UBERSHADER_ONLY);
}

TEST_CASE("[Rendering][PipelinePreparation] Cancellation and reuse retain no draw state") {
	Preparation preparation;
	preparation.request();
	preparation.frame = 7;
	preparation.pipeline_binds = 2;
	preparation.pass_mask = 3;
	preparation.pipeline_policy = Preparation::POLICY_UBERSHADER_ONLY;
	preparation.cancel();
	CHECK(preparation.state == Preparation::CANCELLED);
	CHECK(preparation.request_id == 1);
	CHECK(preparation.frame == 0);
	CHECK(preparation.pipeline_binds == 0);
	CHECK(preparation.pass_mask == 0);
	CHECK(preparation.pipeline_policy == Preparation::POLICY_UNSET);
	// A rendering callback can cancel while the current draw is finishing.
	preparation.render_completed = true;
	preparation.draw_calls = 1;
	preparation.finish();
	CHECK(preparation.state == Preparation::CANCELLED);
	CHECK(preparation.draw_calls == 0);
	CHECK_FALSE(preparation.render_completed);
	preparation.request();
	CHECK(preparation.state == Preparation::PENDING);
	CHECK(preparation.request_id == 2);
	CHECK_FALSE(preparation.renderer_supported);
	CHECK_FALSE(preparation.render_completed);
}

} // namespace TestPipelinePreparation
