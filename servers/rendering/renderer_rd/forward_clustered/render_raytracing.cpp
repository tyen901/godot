/**************************************************************************/
/*  render_raytracing.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/environment_storage.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_TEXTURE_STREAMING_ENABLED
#include "modules/texture_streaming/texture_streaming.h"
#endif

using namespace RendererSceneRenderImplementation;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RenderRaytracing::initialize(RenderForwardClustered *p_owner) {
	owner = p_owner;
#ifdef MODULE_TEXTURE_STREAMING_ENABLED
	texture_streaming_enabled = GLOBAL_GET("rendering/textures/streaming/enabled");
#endif
	bindless_block = memnew(BindlessBlock);

	// Initialize merged MultiMesh BLAS compute shader.
	Vector<String> merge_modes;
	merge_modes.push_back("\n");
	merge_modes.push_back("\n#define MODE_INDEXED\n");
	mm_merge_shader.shader.initialize(merge_modes);
	mm_merge_shader.version = mm_merge_shader.shader.version_create();
	for (int i = 0; i < MergeShader::MODE_MAX; i++) {
		mm_merge_shader.version_shader[i] = mm_merge_shader.shader.version_get_shader(mm_merge_shader.version, i);
		mm_merge_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(mm_merge_shader.version_shader[i]);
	}
}

RenderRaytracing::~RenderRaytracing() {
	for (KeyValue<RenderSceneBuffersRD *, RTViewportState *> &kv : viewport_states) {
		_free_viewport_state_internal(kv.value);
	}
	viewport_states.clear();

	cleanup_caches();
	if (decal_unit_blas.is_valid()) RD::get_singleton()->free_rid(decal_unit_blas);
	if (decal_unit_buffer.is_valid()) RD::get_singleton()->free_rid(decal_unit_buffer);

	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->free_rid(mat_ubo_pool_buffer);
		mat_ubo_pool_buffer = RID();
	}

	if (bindless_block) {
		memdelete(bindless_block);
		bindless_block = nullptr;
	}
	if (shader) {
		memdelete(shader);
		shader = nullptr;
	}

	mm_merge_shader.shader.version_free(mm_merge_shader.version);
}

// ---------------------------------------------------------------------------
// Per-viewport state lifecycle
// ---------------------------------------------------------------------------

RTViewportState *RenderRaytracing::_get_or_create_viewport_state(const RenderDataRD *p_render_data) {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(key);
	if (it != viewport_states.end()) {
		return it->value;
	}
	RTViewportState *state = memnew(RTViewportState);
	viewport_states.insert(key, state);
	return state;
}

RTViewportState *RenderRaytracing::_get_viewport_state(const RenderDataRD *p_render_data) const {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RenderSceneBuffersRD *key = p_render_data->render_buffers.ptr();
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::ConstIterator it = viewport_states.find(key);
	return (it != viewport_states.end()) ? it->value : nullptr;
}

void RenderRaytracing::_free_viewport_state_internal(RTViewportState *p_state) {
	if (!p_state) {
		return;
	}
	if (p_state->decal_tlas.is_valid()) RD::get_singleton()->free_rid(p_state->decal_tlas);
	if (p_state->decal_buffer.is_valid()) RD::get_singleton()->free_rid(p_state->decal_buffer);
	if (p_state->tlas.is_valid()) {
		RD::get_singleton()->free_rid(p_state->tlas);
	}
	if (p_state->geometry_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->geometry_buffer);
	}
	if (p_state->material_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->material_buffer);
	}
	if (p_state->motion_index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_index_buffer);
	}
	if (p_state->motion_transform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_transform_buffer);
	}
	if (p_state->light_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->light_buffer);
	}
	if (p_state->params_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->params_buffer);
	}
	if (p_state->scene_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->scene_uniform_set)) {
		RD::get_singleton()->free_rid(p_state->scene_uniform_set);
	}
	memdelete(p_state);
}

void RenderRaytracing::free_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	HashMap<RenderSceneBuffersRD *, RTViewportState *>::Iterator it = viewport_states.find(p_render_buffers);
	if (it == viewport_states.end()) {
		return;
	}
	_free_viewport_state_internal(it->value);
	viewport_states.remove(it);
}

// ---------------------------------------------------------------------------
// Raytracing / DLSS-RR output textures (stored on the render buffers via named
// scopes) and the per-viewport DLSS upscaler context (in RTViewportState).
// ---------------------------------------------------------------------------

void RenderRaytracing::rt_ensure_textures(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	if (!p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING)) {
		p_render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH)) {
		p_render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
}

bool RenderRaytracing::rt_has_texture(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING);
}

RID RenderRaytracing::rt_get_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING);
}

bool RenderRaytracing::rt_has_depth_texture(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH);
}

RID RenderRaytracing::rt_get_depth_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH);
}

void RenderRaytracing::dlss_rr_ensure_buffers(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	if (p_render_buffers->has_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO)) {
		return;
	}

	uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	// Diffuse Albedo: linear RGB surface color for non-metals (RGBA16F; DLSS-RR expects
	// linear albedo, and float precision avoids 8-bit dark-value banding without gamma packing).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Specular Albedo: RGB specular reflection color (RGBA16F; needs the accuracy, fixes banding artifacts).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_ALBEDO, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Normal + Roughness: World space normals (RGB) + roughness (A).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_NORMAL_ROUGHNESS, RD::DATA_FORMAT_R8G8B8A8_SNORM, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Specular Hit Distance: Single channel distance (R16F is sufficient).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_HIT_DIST, RD::DATA_FORMAT_R16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
}

void RenderRaytracing::dlss_rr_free_buffers(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);
	p_render_buffers->clear_context(RB_SCOPE_DLSS_RR);
}

bool RenderRaytracing::dlss_rr_has_buffers(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_diffuse_albedo(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_specular_albedo(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_normal_roughness(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_NORMAL_ROUGHNESS);
}

RID RenderRaytracing::dlss_rr_get_specular_hit_dist(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_HIT_DIST);
}

// ---------------------------------------------------------------------------
// Material UBO sub-allocation pool
//
// Single device-address storage buffer of MAT_UBO_POOL_TOTAL_BYTES, divided
// into MAT_UBO_POOL_CAPACITY fixed-size slots. Allocate/release just bump a
// next-slot counter and a free-list. Per-material UBO writes are
// buffer_update at slot offset, no allocation. mat.uniform_address is
// pool_bda + slot * slot_size, so the closest-hit shader's
// CustomMaterialUniforms(addr) cast lands on the correct slot.
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t MAT_UBO_POOL_SLOT_SIZE = 512;
constexpr uint32_t MAT_UBO_POOL_CAPACITY = 100000;
constexpr uint64_t MAT_UBO_POOL_TOTAL_BYTES = uint64_t(MAT_UBO_POOL_SLOT_SIZE) * MAT_UBO_POOL_CAPACITY;
} // namespace

void RenderRaytracing::mat_ubo_pool_ensure_initialized() {
	if (mat_ubo_pool_buffer.is_valid()) {
		return;
	}
	Vector<uint8_t> init;
	init.resize(MAT_UBO_POOL_TOTAL_BYTES);
	memset(init.ptrw(), 0, MAT_UBO_POOL_TOTAL_BYTES);
	mat_ubo_pool_buffer = RD::get_singleton()->storage_buffer_create(
			MAT_UBO_POOL_TOTAL_BYTES, init, 0,
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->set_resource_name(mat_ubo_pool_buffer, "RT Material UBO Pool");
		mat_ubo_pool_bda = RD::get_singleton()->buffer_get_device_address(mat_ubo_pool_buffer);
	}
}

uint32_t RenderRaytracing::mat_ubo_pool_allocate() {
	mat_ubo_pool_ensure_initialized();
	if (!mat_ubo_pool_buffer.is_valid()) {
		return UINT32_MAX;
	}
	if (mat_ubo_pool_free_count > 0) {
		--mat_ubo_pool_free_count;
		return mat_ubo_pool_free_slots[mat_ubo_pool_free_count];
	}
	if (mat_ubo_pool_next_slot >= MAT_UBO_POOL_CAPACITY) {
		ERR_PRINT_ONCE("RT Material UBO Pool exhausted; falling back to dedicated buffer.");
		return UINT32_MAX;
	}
	return mat_ubo_pool_next_slot++;
}

void RenderRaytracing::mat_ubo_pool_release(uint32_t p_slot) {
	if (p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	if (mat_ubo_pool_free_count < mat_ubo_pool_free_slots.size()) {
		mat_ubo_pool_free_slots[mat_ubo_pool_free_count] = p_slot;
	} else {
		mat_ubo_pool_free_slots.push_back(p_slot);
	}
	++mat_ubo_pool_free_count;
}

void RenderRaytracing::mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size) {
	if (!mat_ubo_pool_buffer.is_valid() || p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	uint32_t size = MIN(p_size, MAT_UBO_POOL_SLOT_SIZE);
	RD::get_singleton()->buffer_update(mat_ubo_pool_buffer, uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE, size, p_data);
}

uint64_t RenderRaytracing::mat_ubo_pool_get_address(uint32_t p_slot) const {
	return mat_ubo_pool_bda + uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE;
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------

void RenderRaytracing::cleanup_caches() {
	RD *rd = RD::get_singleton();
	for (KeyValue<RTSurfaceKey, RTCacheEntry> &cached : surface_cache) {
		if (cached.value.ptr) {
			if (rd->acceleration_structure_is_valid(cached.value.ptr->blas)) {
				rd->free_rid(cached.value.ptr->blas);
			}
			memdelete(cached.value.ptr);
		}
	}
	surface_cache.clear();

	// Free all cached deformed surface data.
	{
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTDeformedCacheEntry *e = deformed_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			for (KeyValue<uint32_t, RTDeformedCacheEntry::LODVariant> &lod : e->lods) {
				if (lod.value.ptr) {
					if (rd->acceleration_structure_is_valid(lod.value.ptr->blas)) {
						rd->free_rid(lod.value.ptr->blas);
					}
					memdelete(lod.value.ptr);
				}
			}
			e->lods.clear();
			if (e->owned_vb_full.is_valid()) {
				rd->free_rid(e->owned_vb_full);
				e->owned_vb_full = RID();
			}
			if (e->prev_pos_vb.is_valid()) {
				rd->free_rid(e->prev_pos_vb);
				e->prev_pos_vb = RID();
			}
			deformed_pool.free(live[i]);
		}
	}

	// Free all merged MultiMesh BLAS data.
	{
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTMergedMMEntry *e = merged_mm_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->blas.is_valid()) {
				rd->free_rid(e->blas);
				e->blas = RID();
			}
			if (e->merged_vtx_buffer.is_valid()) {
				rd->free_rid(e->merged_vtx_buffer);
				e->merged_vtx_buffer = RID();
			}
			if (e->merged_attr_buffer.is_valid()) {
				rd->free_rid(e->merged_attr_buffer);
				e->merged_attr_buffer = RID();
			}
			if (e->replicated_idx_buffer.is_valid()) {
				rd->free_rid(e->replicated_idx_buffer);
				e->replicated_idx_buffer = RID();
			}
			merged_mm_pool.free(live[i]);
		}
	}
	mm_handles.clear();
	deformed_active_this_frame.clear();
	merged_mm_active_this_frame.clear();

	// Free all cached material data
	for (uint32_t i = 0; i < material_chunks.size(); i++) {
		if (material_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTMaterialCacheEntry *entry = &material_chunks[i][j];
				if (entry->ptr) {
					if (entry->ptr->uniform_buffer.is_valid()) {
						RD::get_singleton()->free_rid(entry->ptr->uniform_buffer);
					}
					if (entry->ptr->uniform_pool_slot != UINT32_MAX) {
						mat_ubo_pool_release(entry->ptr->uniform_pool_slot);
					}
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(material_chunks[i]);
		}
	}
	material_chunks.clear();

	// Reset counters
	material_free_slots.clear();
	next_material_slot = 0;
	vram_used = 0;
	cache_hits = 0;
	cache_misses = 0;
}

// ---------------------------------------------------------------------------
// RID helpers
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::get_rid_index(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() & 0xFFFFFFFFULL);
}

uint32_t RenderRaytracing::get_rid_version(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() >> 32);
}


RTMaterialCacheEntry *RenderRaytracing::get_material_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= material_chunks.size()) {
		material_chunks.push_back(nullptr);
	}

	if (!material_chunks[chunk_idx]) {
		material_chunks.set(chunk_idx, memnew_arr(RTMaterialCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			material_chunks[chunk_idx][i] = RTMaterialCacheEntry();
		}
	}

	return &material_chunks[chunk_idx][entry_idx];
}

uint32_t RenderRaytracing::allocate_material_slot() {
	if (!material_free_slots.is_empty()) {
		uint32_t slot = material_free_slots[material_free_slots.size() - 1];
		material_free_slots.resize(material_free_slots.size() - 1);
		return slot;
	}
	return next_material_slot++;
}

// ---------------------------------------------------------------------------
// Slot-pool access helpers
// ---------------------------------------------------------------------------

RTDeformedCacheEntry *RenderRaytracing::_access_deformed_slot(RID &r_handle) {
	RTDeformedCacheEntry *entry = deformed_pool.get_or_null(r_handle);
	if (!entry) {
		// Stale or never-allocated handle -> fresh slot. RID_Owner default-
		// constructs the entry, so all members start at their cleared values.
		r_handle = deformed_pool.make_rid();
		entry = deformed_pool.get_or_null(r_handle);
	}
	deformed_active_this_frame.push_back(r_handle);
	return entry;
}

RTMergedMMEntry *RenderRaytracing::_access_merged_mm_slot(RID &r_handle) {
	RTMergedMMEntry *entry = merged_mm_pool.get_or_null(r_handle);
	if (!entry) {
		r_handle = merged_mm_pool.make_rid();
		entry = merged_mm_pool.get_or_null(r_handle);
	}
	merged_mm_active_this_frame.push_back(r_handle);
	return entry;
}

// ---------------------------------------------------------------------------
// Per-frame preparation
// ---------------------------------------------------------------------------

void RenderRaytracing::prepare_frame() {
	scene_inputs.clear();
	scene_materials.clear();
	blass.clear();
	blas_transforms.clear();
	instance_flags.clear();
	instance_masks.clear();
	sbt_offsets.clear();
	geometry_data.clear();
	material_data.clear();
	motion_indices.clear();
	motion_transforms.clear();
	deformed_active_this_frame.clear();
	merged_mm_active_this_frame.clear();

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();
	RD *rd = RD::get_singleton();
	surface_active_this_frame.clear();
	static const uint32_t SURFACE_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/mesh_blas_cache_ttl_frames");
	LocalVector<RTSurfaceKey> expired_surfaces;
	for (KeyValue<RTSurfaceKey, RTCacheEntry> &cached : surface_cache) {
		if (current_frame - cached.value.last_used_frame > SURFACE_CACHE_TTL) {
			if (cached.value.ptr) {
				if (rd->acceleration_structure_is_valid(cached.value.ptr->blas)) {
					rd->free_rid(cached.value.ptr->blas);
				}
				memdelete(cached.value.ptr);
			}
			expired_surfaces.push_back(cached.key);
		}
	}
	for (const RTSurfaceKey &key : expired_surfaces) {
		surface_cache.erase(key);
	}

	// TTL-evict stale deformed-surface entries.
	{
		static const uint32_t DEFORMED_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/deformed_mesh_cache_ttl_frames");
		LocalVector<RID> live = deformed_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTDeformedCacheEntry *e = deformed_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->last_used_frame == 0 || current_frame - e->last_used_frame <= DEFORMED_CACHE_TTL) {
				continue;
			}
			for (KeyValue<uint32_t, RTDeformedCacheEntry::LODVariant> &lod : e->lods) {
				if (lod.value.ptr) {
					if (rd->acceleration_structure_is_valid(lod.value.ptr->blas)) {
						rd->free_rid(lod.value.ptr->blas);
					}
					memdelete(lod.value.ptr);
				}
			}
			e->lods.clear();
			if (e->owned_vb_full.is_valid()) {
				rd->free_rid(e->owned_vb_full);
				e->owned_vb_full = RID();
			}
			if (e->prev_pos_vb.is_valid()) {
				rd->free_rid(e->prev_pos_vb);
				e->prev_pos_vb = RID();
			}
			deformed_pool.free(live[i]);
		}
	}

	// TTL-evict stale merged-MultiMesh entries.
	{
		static const uint32_t MM_BLAS_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/multimesh_blas_cache_ttl_frames");
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (uint32_t i = 0; i < live.size(); i++) {
			RTMergedMMEntry *e = merged_mm_pool.get_or_null(live[i]);
			if (!e) {
				continue;
			}
			if (e->last_used_frame == 0 || current_frame - e->last_used_frame <= MM_BLAS_CACHE_TTL) {
				continue;
			}
			if (e->blas.is_valid()) {
				rd->free_rid(e->blas);
				e->blas = RID();
			}
			if (e->merged_vtx_buffer.is_valid()) {
				rd->free_rid(e->merged_vtx_buffer);
				e->merged_vtx_buffer = RID();
			}
			if (e->merged_attr_buffer.is_valid()) {
				rd->free_rid(e->merged_attr_buffer);
				e->merged_attr_buffer = RID();
			}
			if (e->replicated_idx_buffer.is_valid()) {
				rd->free_rid(e->replicated_idx_buffer);
				e->replicated_idx_buffer = RID();
			}
			merged_mm_pool.free(live[i]);
		}
	}

	// Finish async HG compiles so live_ready_mask matches this frame (sync path fills at build_tlas end).
	SceneShaderRaytracing::get_singleton()->drain_completed_compiles();

	// Reset per-frame metrics
	cache_hits = 0;
	cache_misses = 0;

	if (!bindless_block->is_initialized()) {
		bindless_block->initialize(RD::get_singleton());
	}
	bindless_block->begin_frame();
}

// ---------------------------------------------------------------------------
// Surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_surface(
		const void *p_surf,
		void *p_mesh_surface,
		uint32_t p_surface_invalidation_counter,
		uint32_t p_lod,
		const Transform3D &p_transform,
		LocalVector<RID> &r_dirty_blas_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	RID mesh_rid = surf->owner->data->base;
	if (surf->owner->data->base_type == RSE::INSTANCE_MULTIMESH) {
		RID underlying = mesh_storage->multimesh_get_mesh(mesh_rid);
		if (underlying.is_valid()) {
			mesh_rid = underlying;
		}
	}

	uint32_t cache_key = (mesh_rid.get_local_index() << 8) | (surf->surface_index & 0xFF);
	uint32_t mesh_version = get_rid_version(mesh_rid);
	RTSurfaceKey key{ mesh_rid, surf->surface_index, p_lod };
	RTCacheEntry *entry = &surface_cache[key];
	surface_active_this_frame.insert(key);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mesh_version ||
			entry->cached_counter != p_surface_invalidation_counter;

	if (!needs_refresh && RD::get_singleton()->acceleration_structure_is_valid(entry->ptr->blas)) {
		cache_hits++;
		entry->last_used_frame = current_frame;
		return entry->ptr;
	}

	cache_misses++;

	// Allocate or reuse entry
	if (!entry->ptr) {
		entry->ptr = memnew(RTSurfaceData);
	} else {
		if (RD::get_singleton()->acceleration_structure_is_valid(entry->ptr->blas)) {
			RD::get_singleton()->free_rid(entry->ptr->blas);
		}
		entry->ptr->blas = RID();
	}

	RTSurfaceData *surf_data = entry->ptr;

	_populate_surface_blas(p_mesh_surface, RID(), false, false, false, cache_key, p_lod, surf_data, r_dirty_blas_list);

	surf->cached_final_transform_valid = false;

	if (!surf_data->blas.is_valid()) {
		return surf_data;
	}

	entry->cached_counter = p_surface_invalidation_counter;
	entry->cached_rid_version = mesh_version;
	entry->last_used_frame = current_frame;

	return surf_data;
}

// ---------------------------------------------------------------------------
// Deformed surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_deformed_surface(
		const void *p_surf,
		void *p_mesh_surface,
		const RTDeformedGeometrySource &p_source,
		uint32_t p_lod,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);

	if (!p_source.current_vb.is_valid()) {
		return nullptr;
	}

	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	// Compute the deformed VB layout.
	uint64_t fmt = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	bool is_2d = fmt & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	bool has_n = fmt & RSE::ARRAY_FORMAT_NORMAL;
	bool has_t = fmt & RSE::ARRAY_FORMAT_TANGENT;
	uint32_t position_stride = is_2d ? (uint32_t)sizeof(float) * 2u : (uint32_t)sizeof(float) * 3u;
	uint32_t tbn_section_per_vertex = has_n ? (has_t ? 8u : 4u) : 0u;
	uint32_t per_vertex_bytes = position_stride + tbn_section_per_vertex;
	uint32_t full_size = per_vertex_bytes * vertex_count;
	uint32_t pos_size = position_stride * vertex_count;

	if (vertex_count == 0 || full_size == 0) {
		return nullptr;
	}

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	uint64_t buffer_id = p_source.current_vb.get_id();

	RTDeformedCacheEntry *entry_ptr = _access_deformed_slot(surf->rt_deformed_handle);
	ERR_FAIL_NULL_V(entry_ptr, nullptr);
	RTDeformedCacheEntry &entry = *entry_ptr;

	bool layout_changed = entry.cached_vertex_count != vertex_count || entry.cached_full_size != full_size;
	bool data_changed = layout_changed ||
			entry.cached_change_stamp != p_source.change_stamp ||
			entry.cached_buffer_id != buffer_id;
	const bool source_changed = layout_changed || entry.cached_key_version != p_source.cache_version ||
			entry.cached_surface_counter != p_source.surface_counter || entry.cached_buffer_id != buffer_id;
	if (source_changed) {
		for (KeyValue<uint32_t, RTDeformedCacheEntry::LODVariant> &lod : entry.lods) {
			if (lod.value.ptr) {
				if (rd->acceleration_structure_is_valid(lod.value.ptr->blas)) {
					rd->free_rid(lod.value.ptr->blas);
				}
				memdelete(lod.value.ptr);
			}
		}
		entry.lods.clear();
	}
	RTDeformedCacheEntry::LODVariant &variant = entry.lods[p_lod];
	bool needs_full_rebuild = !variant.ptr || !rd->acceleration_structure_is_valid(variant.ptr->blas);

	BitField<RD::BufferCreationBits> owned_full_flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;
	BitField<RD::BufferCreationBits> prev_flags = RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;

	// Reallocate owned storage if the surface grew.
	if (entry.owned_vb_full_capacity < full_size) {
		if (entry.owned_vb_full.is_valid()) {
			rd->free_rid(entry.owned_vb_full);
		}
		entry.owned_vb_full = rd->vertex_buffer_create(full_size, Span<uint8_t>(), owned_full_flags);
		ERR_FAIL_COND_V(!entry.owned_vb_full.is_valid(), nullptr);
		entry.owned_vb_full_capacity = full_size;
		rd->set_resource_name(entry.owned_vb_full, "RT deformed owned VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
		needs_full_rebuild = true;
	}
	if (entry.prev_pos_vb_capacity < pos_size) {
		if (entry.prev_pos_vb.is_valid()) {
			rd->free_rid(entry.prev_pos_vb);
		}
		entry.prev_pos_vb = rd->storage_buffer_create(pos_size, Vector<uint8_t>(), 0, prev_flags);
		ERR_FAIL_COND_V(!entry.prev_pos_vb.is_valid(), nullptr);
		entry.prev_pos_vb_capacity = pos_size;
		rd->set_resource_name(entry.prev_pos_vb, "RT deformed prev pos VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
	}

	// Per-frame copies run exactly once per frame, even when the same surface is visible from multiple viewports.
	const bool first_touch_this_frame = entry.last_used_frame != current_frame;
	if (first_touch_this_frame || data_changed) {
		if (first_touch_this_frame && entry.prev_pos_seeded) {
			rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
		}
		if (data_changed || !entry.prev_pos_seeded) {
			rd->buffer_copy(p_source.current_vb, entry.owned_vb_full, 0, 0, full_size);
		}
		if (!entry.prev_pos_seeded) {
			// Seed prev = current so motion vectors are zero on the first frame.
			rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
			entry.prev_pos_seeded = true;
		}
	}

	if (needs_full_rebuild) {
		cache_misses++;
		if (!variant.ptr) {
			variant.ptr = memnew(RTSurfaceData);
		}
		if (rd->acceleration_structure_is_valid(variant.ptr->blas)) {
			rd->free_rid(variant.ptr->blas);
			variant.ptr->blas = RID();
		}
		_populate_surface_blas(p_mesh_surface, entry.owned_vb_full, true, true, true,
				static_cast<uint32_t>(p_source.cache_key), p_lod, variant.ptr, r_dirty_blas_list);
		variant.change_stamp = p_source.change_stamp;
	} else if (variant.change_stamp != p_source.change_stamp) {
		cache_misses++;
		r_dirty_blas_update_list.push_back(variant.ptr->blas);
		variant.change_stamp = p_source.change_stamp;
	} else {
		cache_hits++;
	}

	if (variant.ptr && variant.ptr->blas.is_valid()) {
		variant.ptr->geometry.flags |= RT_GEOM_FLAG_DEFORMED;
		uint64_t prev_addr = rd->buffer_get_device_address(entry.prev_pos_vb);
		variant.ptr->geometry.prev_vertex_buffer_address_lo = static_cast<uint32_t>(prev_addr & 0xFFFFFFFFULL);
		variant.ptr->geometry.prev_vertex_buffer_address_hi = static_cast<uint32_t>(prev_addr >> 32);
	}

	surf->cached_final_transform_valid = false;

	entry.cached_change_stamp = p_source.change_stamp;
	entry.cached_key_version = p_source.cache_version;
	entry.cached_surface_counter = p_source.surface_counter;
	entry.cached_buffer_id = buffer_id;
	entry.cached_vertex_count = vertex_count;
	entry.cached_full_size = full_size;
	entry.last_used_frame = current_frame;

	return variant.ptr;
}

// ---------------------------------------------------------------------------
// Surface BLAS helper (shared between static and deformed paths)
// ---------------------------------------------------------------------------

// Fills RTSurfaceData geometry metadata from the surface format.
// Returns the RIDs needed for BLAS creation so _populate_surface_blas can use them.
static void _fill_surface_geometry_data(
		void *p_mesh_surface,
		uint32_t p_lod,
		bool p_force_uncompressed,
		RTSurfaceData *r_surf_data,
		RID *r_vertex_buffer = nullptr,
		RID *r_attribute_buffer = nullptr,
		RID *r_index_buffer = nullptr) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	bool compressed = (surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) && !p_force_uncompressed;
	bool is_2d = surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES;

	r_surf_data->is_compressed = compressed;

	if (compressed) {
		AABB surface_aabb = mesh_storage->mesh_surface_get_aabb(p_mesh_surface);
		r_surf_data->aabb_transform.basis = Basis::from_scale(surface_aabb.size);
		r_surf_data->aabb_transform.origin = surface_aabb.position;
	} else {
		r_surf_data->aabb_transform = Transform3D();
	}

	RT_GeometryData &geom = r_surf_data->geometry;
	memset(&geom, 0, sizeof(geom));

	RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	RID attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, p_lod);

	if (r_vertex_buffer) {
		*r_vertex_buffer = vertex_buffer;
	}
	if (r_attribute_buffer) {
		*r_attribute_buffer = attribute_buffer;
	}
	if (r_index_buffer) {
		*r_index_buffer = index_buffer;
	}

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, p_lod);

	geom.vertex_count = vertex_count;

	// Position stride
	uint32_t position_stride;
	if (is_2d) {
		position_stride = sizeof(float) * 2;
	} else if (compressed) {
		position_stride = sizeof(uint16_t) * 4;
	} else {
		position_stride = sizeof(float) * 3;
	}
	geom.position_stride = position_stride;

	// Normal/tangent layout
	uint32_t normal_stride;
	uint32_t tangent_stride = 0;
	geom.normal_byte_offset = RT_OFFSET_NONE;
	geom.tangent_byte_offset = RT_OFFSET_NONE;
	uint32_t current_offset = position_stride * vertex_count;

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;

	if (compressed) {
		normal_stride = sizeof(uint16_t) * 2;
		if (has_normal) {
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		}
	} else {
		if (has_normal && has_tangent) {
			normal_stride = sizeof(uint16_t) * 4;
			tangent_stride = sizeof(uint16_t) * 4;
			geom.normal_byte_offset = current_offset;
			geom.tangent_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else if (has_normal) {
			normal_stride = sizeof(uint16_t) * 2;
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else {
			normal_stride = 0;
		}
	}
	geom.normal_stride = normal_stride;
	geom.tangent_stride = tangent_stride;
	geom.flags = compressed ? RT_GEOM_FLAG_COMPRESSED : 0;

	if (compressed) {
		AABB surface_aabb = mesh_storage->mesh_surface_get_aabb(p_mesh_surface);
		geom.aabb_size_x = surface_aabb.size.x;
		geom.aabb_size_y = surface_aabb.size.y;
		geom.aabb_size_z = surface_aabb.size.z;
		geom.aabb_pos_x = surface_aabb.position.x;
		geom.aabb_pos_y = surface_aabb.position.y;
		geom.aabb_pos_z = surface_aabb.position.z;
	} else {
		geom.aabb_size_x = 1.0f;
		geom.aabb_size_y = 1.0f;
		geom.aabb_size_z = 1.0f;
		geom.aabb_pos_x = 0.0f;
		geom.aabb_pos_y = 0.0f;
		geom.aabb_pos_z = 0.0f;
	}

	// Attribute buffer layout
	uint32_t attrib_offset = 0;
	geom.uv_byte_offset = RT_OFFSET_NONE;
	geom.color_byte_offset = RT_OFFSET_NONE;

	if (surface_format & RSE::ARRAY_FORMAT_COLOR) {
		geom.color_byte_offset = attrib_offset;
		attrib_offset += sizeof(uint32_t);
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV) {
		geom.uv_byte_offset = attrib_offset;
		attrib_offset += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV2) {
		attrib_offset += compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	for (int ci = 0; ci < RSE::ARRAY_CUSTOM_COUNT; ci++) {
		geom.custom_attribute[ci] = RT_OFFSET_NONE;
		const uint32_t fmt_shift[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
		if (surface_format & (1ULL << (RSE::ARRAY_CUSTOM0 + ci))) {
			uint32_t fmt = (surface_format >> fmt_shift[ci]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
			geom.custom_attribute[ci] = attrib_offset | (fmt << 28);
			const uint32_t fmtsize[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
			attrib_offset += fmtsize[fmt];
		}
	}
	geom.attribute_stride = attrib_offset;

	// UV scale (fp16 packed, matches GLSL unpackHalf2x16)
	Vector4 uv_scale = mesh_storage->mesh_surface_get_uv_scale(p_mesh_surface);
	geom.uv_scale_packed = (uint32_t(Math::make_half_float(uv_scale.y)) << 16) | Math::make_half_float(uv_scale.x);

	// Index format (no device address — caller fills those in)
	if (index_buffer.is_valid() && index_count > 0) {
		bool is_16bit = vertex_count <= 65536 && vertex_count > 0;
		geom.index_format = is_16bit ? RT_INDEX_FORMAT_UINT16 : RT_INDEX_FORMAT_UINT32;
		geom.primitive_count = index_count / 3;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = vertex_count / 3;
	}
}

void RenderRaytracing::_populate_surface_blas(
		void *p_mesh_surface,
		RID p_vertex_buffer_override,
		bool p_force_uncompressed,
		bool p_prefer_fast_build,
		bool p_allow_update,
		uint32_t p_cache_key,
		uint32_t p_lod,
		RTSurfaceData *r_surf_data,
		LocalVector<RID> &r_dirty_blas_list) {
	RID vertex_buffer, attribute_buffer, index_buffer;
	_fill_surface_geometry_data(p_mesh_surface, p_lod, p_force_uncompressed, r_surf_data,
			&vertex_buffer, &attribute_buffer, &index_buffer);

	if (p_vertex_buffer_override.is_valid()) {
		vertex_buffer = p_vertex_buffer_override;
	}

	RD *rd = RD::get_singleton();
	RT_GeometryData &geom = r_surf_data->geometry;

	if (vertex_buffer.is_valid()) {
		geom.vertex_buffer_address = rd->buffer_get_device_address(vertex_buffer);
	}
	if (attribute_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(attribute_buffer);
	}
	if (index_buffer.is_valid() && geom.index_format != RT_INDEX_FORMAT_NONE) {
		geom.index_buffer_address = rd->buffer_get_device_address(index_buffer);
	}

	uint32_t vertex_count = geom.vertex_count;
	uint32_t index_count = (geom.index_format != RT_INDEX_FORMAT_NONE) ? geom.primitive_count * 3 : 0;
	uint32_t position_stride = geom.position_stride;

	bool is_2d = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(p_mesh_surface) & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	bool compressed = r_surf_data->is_compressed;

	// Create BLAS using the new geometry-based API.
	{
		RD::DataFormat pos_format;
		if (is_2d) {
			pos_format = RD::DATA_FORMAT_R32G32_SFLOAT;
		} else if (compressed) {
			pos_format = RD::DATA_FORMAT_R16G16B16A16_UNORM;
		} else {
			pos_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		}

		RD::AccelerationStructureGeometry as_geom;
		// Type defaults to TYPE_TRIANGLES; set explicitly for clarity.
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = vertex_buffer;
		as_geom.geometry.triangles.vertex_stride = position_stride;
		as_geom.geometry.triangles.vertex_count = vertex_count;
		as_geom.geometry.triangles.vertex_format = pos_format;

		if (index_buffer.is_valid() && index_count > 0) {
			as_geom.geometry.triangles.index_buffer = index_buffer;
			as_geom.geometry.triangles.index_count = index_count;
		}

		BitField<RD::AccelerationStructureFlagBits> as_flags = p_prefer_fast_build
				? RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT
				: RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
		if (p_allow_update) {
			as_flags.set_flag(RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
		}

		r_surf_data->blas = RD::get_singleton()->blas_create({ &as_geom, 1 }, as_flags);
		if (!r_surf_data->blas.is_valid()) {
			return;
		}
		RD::get_singleton()->set_resource_name(r_surf_data->blas,
				String(p_vertex_buffer_override.is_valid() ? "RT BLAS deformed [" : "RT BLAS [") + itos(p_cache_key) + "]");
		r_dirty_blas_list.push_back(r_surf_data->blas);
	}
}

// ---------------------------------------------------------------------------
// Procedural geometry processing
// ---------------------------------------------------------------------------

void RenderRaytracing::update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list) {
	// Pack AABB data into a byte buffer.
	Vector<uint8_t> aabb_bytes;
	uint32_t aabb_count = 1;

	if (p_state->aabb_data.size() >= 6 && (p_state->aabb_data.size() % 6) == 0) {
		aabb_count = p_state->aabb_data.size() / 6;
		aabb_bytes.resize(p_state->aabb_data.size() * sizeof(float));
		memcpy(aabb_bytes.ptrw(), p_state->aabb_data.ptr(), aabb_bytes.size());
	} else {
		const AABB &a = p_state->culling_aabb;
		float single[6] = {
			(float)a.position.x, (float)a.position.y, (float)a.position.z,
			(float)(a.position.x + a.size.x), (float)(a.position.y + a.size.y), (float)(a.position.z + a.size.z)
		};
		aabb_bytes.resize(sizeof(single));
		memcpy(aabb_bytes.ptrw(), single, sizeof(single));
	}

	uint32_t required_bytes = aabb_bytes.size();
	bool needs_new_blas = false;

	// Grow-only: only recreate the buffer when capacity is exceeded or count changed.
	if (required_bytes > p_state->gpu_buffer_capacity || aabb_count != p_state->aabb_count) {
		if (p_state->blas.is_valid()) {
			RD::get_singleton()->free_rid(p_state->blas);
			p_state->blas = RID();
		}
		if (p_state->gpu_buffer.is_valid()) {
			RD::get_singleton()->free_rid(p_state->gpu_buffer);
		}
		p_state->gpu_buffer = RD::get_singleton()->storage_buffer_create(required_bytes, aabb_bytes,
				0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		p_state->gpu_buffer_capacity = required_bytes;
		p_state->aabb_count = aabb_count;
		needs_new_blas = true;
	} else {
		// Buffer is large enough -- just update contents.
		RD::get_singleton()->buffer_update(p_state->gpu_buffer, 0, required_bytes, aabb_bytes.ptr());
		needs_new_blas = !p_state->blas.is_valid();
	}

	if (needs_new_blas) {
		ERR_FAIL_COND(!p_state->gpu_buffer.is_valid());

		RD::AccelerationStructureGeometry geom;
		geom.type = RD::AccelerationStructureGeometry::TYPE_AABBS;
		geom.geometry.aabbs.buffer = p_state->gpu_buffer;
		geom.geometry.aabbs.count = aabb_count;
		geom.geometry.aabbs.stride = 24; // VkAabbPositionsKHR: two float3 (min, max).
		p_state->blas = RD::get_singleton()->blas_create({ &geom, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	}

	// BDA for shader access.
	if (p_state->expose_bounds && p_state->gpu_buffer.is_valid()) {
		p_state->gpu_buffer_address = RD::get_singleton()->buffer_get_device_address(p_state->gpu_buffer);
	} else {
		p_state->gpu_buffer_address = 0;
	}

	if (p_state->blas.is_valid()) {
		r_dirty_blas_list.push_back(p_state->blas);
	}
}

// ---------------------------------------------------------------------------
// Material processing
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::_get_material_feedback_index(RID p_material) const {
#ifdef MODULE_TEXTURE_STREAMING_ENABLED
	TextureStreaming *streaming = TextureStreaming::get_singleton();
	if (texture_streaming_enabled && streaming && streaming->feedback_buffer_get_uniform_rid().is_valid()) {
		RendererRD::MaterialStorage::MaterialData *material = RendererRD::MaterialStorage::get_singleton()->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D);
		if (material && material->material_feedback_rid.is_valid()) {
			// Register in the current native readback buffer on every frame, including
			// static TLAS reuse. MaterialData owns the feedback RID and its textures.
			return streaming->feedback_buffer_material_index(material->material_feedback_rid);
		}
	}
#endif
	return UINT32_MAX;
}

RTMaterialData *RenderRaytracing::process_material(RID p_material_rid, uint64_t p_material_invalidation_counter) {
	// Static default material for invalid/null materials
	static RTMaterialData s_default_mat;
	static bool s_default_mat_initialized = false;
	if (!s_default_mat_initialized) {
		s_default_mat.data.albedo_color[0] = 1.0f;
		s_default_mat.data.albedo_color[1] = 1.0f;
		s_default_mat.data.albedo_color[2] = 1.0f;
		s_default_mat.data.albedo_color[3] = 1.0f;
		s_default_mat.data.emission_color[0] = 0.0f;
		s_default_mat.data.emission_color[1] = 0.0f;
		s_default_mat.data.emission_color[2] = 0.0f;
		s_default_mat.data.emission_strength = 0.0f;
		s_default_mat.data.roughness = 1.0f;
		s_default_mat.data.specular = 0.5f;
		s_default_mat.data.ao_strength = 1.0f;
		s_default_mat.data.uv1_scale[0] = 1.0f;
		s_default_mat.data.uv1_scale[1] = 1.0f;
		s_default_mat.data.uv1_offset[0] = 0.0f;
		s_default_mat.data.uv1_offset[1] = 0.0f;
		s_default_mat_initialized = true;
	}

	if (!p_material_rid.is_valid()) {
		return &s_default_mat;
	}

	const uint32_t material_feedback_index = _get_material_feedback_index(p_material_rid);

	// Cache lookup
	uint32_t mat_idx = get_rid_index(p_material_rid);
	uint32_t mat_version = get_rid_version(p_material_rid);
	RTMaterialCacheEntry *entry = get_material_cache_entry(mat_idx);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mat_version ||
			entry->cached_counter != p_material_invalidation_counter;

	if (!needs_refresh) {
		entry->ptr->data.material_feedback_index = material_feedback_index;
		entry->last_used_frame = current_frame;
		if (entry->ptr->is_custom_shader) {
			uint32_t shader_id = RendererRD::MaterialStorage::get_singleton()->material_get_shader_id(p_material_rid);
			uint32_t old_sbt = entry->ptr->rt_sbt_offset;
			uint32_t new_sbt = SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid);
			entry->ptr->rt_sbt_offset = new_sbt;
			// HG slot change invalidates cached UBO layout / BDA.
			if (old_sbt != new_sbt) {
				needs_refresh = true;
			}
		}
		if (!needs_refresh) {
			scene_materials.insert(p_material_rid, { p_material_invalidation_counter, entry->ptr->rt_sbt_offset, entry->ptr->data });
			return entry->ptr;
		}
	}

	// Cache miss - need to rebuild material
	if (!entry->ptr) {
		entry->ptr = memnew(RTMaterialData);
	}

	RTMaterialData *mat_data = entry->ptr;
	RT_MaterialData &mat = mat_data->data;

	// Initialize defaults
	mat.albedo_color[0] = 1.0f;
	mat.albedo_color[1] = 1.0f;
	mat.albedo_color[2] = 1.0f;
	mat.albedo_color[3] = 1.0f;
	mat.emission_color[0] = 0.0f;
	mat.emission_color[1] = 0.0f;
	mat.emission_color[2] = 0.0f;
	mat.emission_strength = 0.0f;
	mat.metallic = 0.0f;
	mat.roughness = 1.0f;
	mat.specular = 0.5f;
	mat.ao_strength = 1.0f;
	mat.flags = 0;
	mat.albedo_texture_idx = 0;
	mat.normal_texture_idx = 0;
	mat.orm_texture_idx = 0;
	mat.emission_texture_idx = 0;
	mat.uv1_scale[0] = 1.0f;
	mat.uv1_scale[1] = 1.0f;
	mat.uv1_offset[0] = 0.0f;
	mat.uv1_offset[1] = 0.0f;
	mat.normal_map_depth = 1.0f;
	mat.uniform_address = 0;
	mat.material_feedback_index = material_feedback_index;

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	// Helper lambda to get texture from material parameter
	// p_srgb should be true for color textures (albedo, emission) that need sRGB->linear conversion
	auto get_material_texture = [&](const StringName &p_param, bool p_srgb = false) -> RID {
		Variant tex_var = material_storage->material_get_param(p_material_rid, p_param);
		if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
			RID tex_rid = tex_var;
			if (tex_rid.is_valid()) {
				return texture_storage->texture_get_rd_texture(tex_rid, p_srgb);
			}
		}
		return RID();
	};

	// Textures
	// Albedo is a color texture - needs sRGB->linear conversion
	RID albedo_rd = get_material_texture("texture_albedo", true);
	if (albedo_rd.is_valid()) {
		mat.albedo_texture_idx = bindless_block->add_texture(albedo_rd);
	}

	RID normal_rd = get_material_texture("texture_normal");
	if (normal_rd.is_valid()) {
		mat.normal_texture_idx = bindless_block->add_texture(normal_rd);
		mat.flags |= RT_MAT_FLAG_HAS_NORMAL_MAP;

		Variant normal_scale_var = material_storage->material_get_param(p_material_rid, "normal_scale");
		if (normal_scale_var.get_type() == Variant::FLOAT) {
			mat.normal_map_depth = normal_scale_var;
		}
	}

	RID orm_rd = get_material_texture("texture_orm");
	if (orm_rd.is_valid()) {
		mat.orm_texture_idx = bindless_block->add_texture(orm_rd);
	} else {
		RID roughness_rd = get_material_texture("texture_roughness");
		if (roughness_rd.is_valid()) {
			mat.orm_texture_idx = bindless_block->add_texture(roughness_rd);
		}
	}

	// Emission is a color texture - needs sRGB->linear conversion
	RID emission_rd = get_material_texture("texture_emission", true);
	if (emission_rd.is_valid()) {
		mat.emission_texture_idx = bindless_block->add_texture(emission_rd);
		mat.flags |= RT_MAT_FLAG_HAS_EMISSION_TEX;
		// Set sensible defaults for emission when texture is present
		mat.emission_color[0] = 1.0f;
		mat.emission_color[1] = 1.0f;
		mat.emission_color[2] = 1.0f;
		mat.emission_strength = 1.0f;
	}

	// Material properties
	// Colors declared with source_color in Godot shaders are stored in sRGB;
	// material_get_param returns the raw sRGB value, so we convert to linear here.
	Variant albedo_var = material_storage->material_get_param(p_material_rid, "albedo");
	if (albedo_var.get_type() == Variant::COLOR) {
		Color c = ((Color)albedo_var).srgb_to_linear();
		mat.albedo_color[0] = c.r;
		mat.albedo_color[1] = c.g;
		mat.albedo_color[2] = c.b;
		mat.albedo_color[3] = c.a;
		mat_data->rt_sbt_offset = 0;
		mat_data->is_custom_shader = false;
	} else {
		mat_data->is_custom_shader = true;
		uint32_t shader_id = material_storage->material_get_shader_id(p_material_rid);
		mat_data->rt_sbt_offset = SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid);

		const SceneShaderRaytracing::CustomShaderEntry *cse =
				SceneShaderRaytracing::get_singleton()->get_custom_shader_entry(mat_data->rt_sbt_offset);
		if (cse && cse->uniform_total_size > 0) {
			Vector<uint8_t> ubo_data;
			ubo_data.resize(cse->uniform_total_size);
			memset(ubo_data.ptrw(), 0, cse->uniform_total_size);

			for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &kv : cse->uniforms) {
				const ShaderLanguage::ShaderNode::Uniform &u = kv.value;
				if (ShaderLanguage::is_sampler_type(u.type) || u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
					continue;
				}
				if (u.order < 0 || u.order >= (int)cse->uniform_offsets.size()) {
					continue;
				}

				uint32_t offset = cse->uniform_offsets[u.order];
				uint32_t size = SceneShaderRaytracing::uniform_std140_size(u);
				if (offset + size > cse->uniform_total_size) {
					continue;
				}

				uint8_t *dst = ubo_data.ptrw() + offset;

				if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
					int32_t idx = material_storage->global_shader_uniform_get_buffer_index(kv.key);
					uint32_t uidx = (idx >= 0) ? (uint32_t)idx : 0;
					memcpy(dst, &uidx, sizeof(uint32_t));
				} else {
					// The same std140 packer as the raster's material buffer, so a
					// source_color default, an unhinted color and an array read the
					// same bytes in the hit shader as in the fragment shader.
					Variant val = material_storage->material_get_param(p_material_rid, kv.key);
					RendererRD::MaterialStorage::fill_std140_uniform(u, val, dst, true);
				}
			}

			RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
			for (int ti = 0; ti < cse->texture_uniforms.size(); ti++) {
				const SceneShaderRaytracing::TextureUniformInfo &tui = cse->texture_uniforms[ti];
				uint32_t bindless_idx = 0;

				if (tui.is_global) {
					RID tex_rid = material_storage->global_shader_uniform_get_texture(tui.name);
					if (tex_rid.is_valid()) {
						RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
						if (rd_tex.is_valid()) {
							bindless_idx = bindless_block->add_texture(rd_tex);
						}
					}
				} else {
					Variant tex_var = material_storage->material_get_param(p_material_rid, tui.name);
					if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
						RID tex_rid = tex_var;
						if (tex_rid.is_valid()) {
							RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
							if (rd_tex.is_valid()) {
								bindless_idx = bindless_block->add_texture(rd_tex);
							}
						}
					}
				}

				if (bindless_idx == 0 && tui.hint != ShaderLanguage::ShaderNode::Uniform::HINT_NONE) {
					using Hint = ShaderLanguage::ShaderNode::Uniform::Hint;
					RID default_tex;
					switch (tui.hint) {
						case Hint::HINT_DEFAULT_BLACK:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
							break;
						case Hint::HINT_DEFAULT_TRANSPARENT:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_TRANSPARENT);
							break;
						case Hint::HINT_NORMAL:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
							break;
						case Hint::HINT_ANISOTROPY:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_ANISO);
							break;
						default:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
							break;
					}
					if (default_tex.is_valid()) {
						bindless_idx = bindless_block->add_texture(default_tex);
					}
				}

				if (tui.buffer_offset + 4 <= cse->uniform_total_size) {
					memcpy(ubo_data.ptrw() + tui.buffer_offset, &bindless_idx, 4);
				}
			}

			// If the shader declared hint_alpha, mirror that bindless index into
			// mat.albedo_texture_idx so ray_query_alpha_test() can sample it cheaply.
			if (cse->alpha_texture_buffer_offset != UINT32_MAX &&
					cse->alpha_texture_buffer_offset + 4 <= cse->uniform_total_size) {
				uint32_t alpha_idx = 0;
				memcpy(&alpha_idx, ubo_data.ptr() + cse->alpha_texture_buffer_offset, 4);
				mat.albedo_texture_idx = alpha_idx;
			}

			// Try the suballoc pool first. Common materials (UBO <= slot size)
			// just buffer_update an existing slot - O(1), no driver allocation,
			// no per-frame storage_buffer_create cost.
			bool used_pool = false;
			if (cse->uniform_total_size <= MAT_UBO_POOL_SLOT_SIZE) {
				if (mat_data->uniform_pool_slot == UINT32_MAX) {
					mat_data->uniform_pool_slot = mat_ubo_pool_allocate();
				}
				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					// Transitioning from a dedicated buffer back into the pool.
					if (mat_data->uniform_buffer.is_valid()) {
						RD::get_singleton()->free_rid(mat_data->uniform_buffer);
						mat_data->uniform_buffer = RID();
					}
					mat_ubo_pool_update(mat_data->uniform_pool_slot, ubo_data.ptr(), cse->uniform_total_size);
					mat.uniform_address = mat_ubo_pool_get_address(mat_data->uniform_pool_slot);
					used_pool = true;
				}
			}

			if (!used_pool) {
				// Larger materials retain a reusable device-address buffer.

				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					mat_ubo_pool_release(mat_data->uniform_pool_slot);
					mat_data->uniform_pool_slot = UINT32_MAX;
				}
				if (mat_data->uniform_buffer.is_valid() && mat_data->uniform_buffer_size != cse->uniform_total_size) {
					RD::get_singleton()->free_rid(mat_data->uniform_buffer);
					mat_data->uniform_buffer = RID();
				}
				if (mat_data->uniform_buffer.is_null()) {
					mat_data->uniform_buffer = RD::get_singleton()->storage_buffer_create(cse->uniform_total_size, ubo_data, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
					mat_data->uniform_buffer_size = cse->uniform_total_size;
				} else {
					RD::get_singleton()->buffer_update(mat_data->uniform_buffer, 0, cse->uniform_total_size, ubo_data.ptr());
				}
				RD::get_singleton()->set_resource_name(mat_data->uniform_buffer, String("RT Material UBO [sbt=") + itos(mat_data->rt_sbt_offset) + "]");
				mat.uniform_address = RD::get_singleton()->buffer_get_device_address(mat_data->uniform_buffer);
			}
		}
	}

	Variant metallic_var = material_storage->material_get_param(p_material_rid, "metallic");
	if (metallic_var.get_type() == Variant::FLOAT) {
		mat.metallic = metallic_var;
	}

	Variant roughness_var = material_storage->material_get_param(p_material_rid, "roughness");
	if (roughness_var.get_type() == Variant::FLOAT) {
		mat.roughness = roughness_var;
	}

	Variant specular_var = material_storage->material_get_param(p_material_rid, "specular");
	if (specular_var.get_type() == Variant::FLOAT) {
		mat.specular = specular_var;
	}

	Variant emission_var = material_storage->material_get_param(p_material_rid, "emission");
	if (emission_var.get_type() == Variant::COLOR) {
		Color c = ((Color)emission_var).srgb_to_linear();
		mat.emission_color[0] = c.r;
		mat.emission_color[1] = c.g;
		mat.emission_color[2] = c.b;
	}

	Variant emission_energy_var = material_storage->material_get_param(p_material_rid, "emission_energy");
	if (emission_energy_var.get_type() == Variant::FLOAT) {
		mat.emission_strength = emission_energy_var;
	}

	// UV1 scale and offset (vec3 in Godot, we only use xy).
	Variant uv1_scale_var = material_storage->material_get_param(p_material_rid, "uv1_scale");
	if (uv1_scale_var.get_type() == Variant::VECTOR3) {
		Vector3 s = uv1_scale_var;
		mat.uv1_scale[0] = s.x;
		mat.uv1_scale[1] = s.y;
	}

	Variant uv1_offset_var = material_storage->material_get_param(p_material_rid, "uv1_offset");
	if (uv1_offset_var.get_type() == Variant::VECTOR3) {
		Vector3 o = uv1_offset_var;
		mat.uv1_offset[0] = o.x;
		mat.uv1_offset[1] = o.y;
	}

	// Point filtering: check if material requests nearest filtering (e.g. pixel art).
	// BaseMaterial3D exposes this as "texture_filter" int param (0=nearest, 1=linear, etc.).
	Variant filter_var = material_storage->material_get_param(p_material_rid, "texture_filter");
	if (filter_var.get_type() == Variant::INT) {
		int filter_mode = filter_var;
		// 0 = TEXTURE_FILTER_NEAREST, 2 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
		// 4 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC
		if (filter_mode == 0 || filter_mode == 2 || filter_mode == 4) {
			mat.flags |= RT_MAT_FLAG_POINT_FILTER;
		}
	}

	// Update cache entry
	entry->cached_counter = p_material_invalidation_counter;
	entry->cached_rid_version = mat_version;
	entry->last_used_frame = current_frame;

	scene_materials.insert(p_material_rid, { p_material_invalidation_counter, mat_data->rt_sbt_offset, mat_data->data });
	return mat_data;
}

// ---------------------------------------------------------------------------
// Acceleration structure building
// ---------------------------------------------------------------------------

Error RenderRaytracing::build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list) {
	RENDER_TIMESTAMP("BLAS Build");

	for (const RID &blas_rid : p_dirty_blas_list) {
		if (blas_rid.is_valid()) {
			ERR_FAIL_COND_V(RD::get_singleton()->blas_build(blas_rid) != OK, ERR_CANT_CREATE);
		}
	}

	for (const RID &blas_rid : p_dirty_blas_update_list) {
		if (blas_rid.is_valid()) {
			ERR_FAIL_COND_V(RD::get_singleton()->blas_update(blas_rid) != OK, ERR_CANT_CREATE);
		}
	}

	RENDER_TIMESTAMP("TLAS Build");

	uint32_t needed = MAX(blass.size(), (uint32_t)1);
	if (!p_state->tlas.is_valid() || needed > p_state->tlas_max_instances) {
		if (p_state->tlas.is_valid()) {
			RD::get_singleton()->free_rid(p_state->tlas);
		}
		p_state->tlas_max_instances = needed * 2;
		p_state->tlas = RD::get_singleton()->tlas_create(p_state->tlas_max_instances, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		RD::get_singleton()->set_resource_name(p_state->tlas, "RT TLAS");
	}

	LocalVector<RD::AccelerationStructureInstance> instances;
	instances.resize(blass.size());
	for (uint32_t i = 0; i < blass.size(); i++) {
		RD::AccelerationStructureInstance &inst = instances[i];
		inst.id = i;
		inst.transform = blas_transforms[i];
		inst.blas = blass[i];
		inst.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(instance_flags[i]);
		inst.mask = (i < instance_masks.size()) ? instance_masks[i] : 0xFF;
		uint32_t sbt_off = (i < sbt_offsets.size()) ? sbt_offsets[i] : 0;
		inst.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | uint64_t(sbt_off));
	}

	return RD::get_singleton()->tlas_build(p_state->tlas, instances);
}

void RenderRaytracing::finalize_buffers(RTViewportState *p_state) {
	// Grow-only uploads. Callers must not free these in prepare_frame().
	auto update_or_grow = [](RID &p_buffer, uint32_t &p_capacity, const void *p_data, uint32_t p_size) {
		if (p_size == 0) {
			return;
		}
		if (p_size > p_capacity) {
			if (p_buffer.is_valid()) {
				RD::get_singleton()->free_rid(p_buffer);
			}
			p_capacity = p_size;
			Vector<uint8_t> init;
			init.resize(p_size);
			memcpy(init.ptrw(), p_data, p_size);
			p_buffer = RD::get_singleton()->storage_buffer_create(p_size, init);
		} else {
			RD::get_singleton()->buffer_update(p_buffer, 0, p_size, p_data);
		}
	};

	update_or_grow(p_state->geometry_buffer, p_state->geometry_buffer_capacity,
			geometry_data.ptr(), geometry_data.size() * sizeof(RT_GeometryData));
	update_or_grow(p_state->material_buffer, p_state->material_buffer_capacity,
			material_data.ptr(), material_data.size() * sizeof(RT_MaterialData));
	update_or_grow(p_state->motion_index_buffer, p_state->motion_index_buffer_capacity,
			motion_indices.ptr(), motion_indices.size() * sizeof(int32_t));
	update_or_grow(p_state->motion_transform_buffer, p_state->motion_transform_buffer_capacity,
			motion_transforms.ptr(), motion_transforms.size() * sizeof(RT_InstanceMotionData));
}

// ---------------------------------------------------------------------------
// Merged MultiMesh BLAS builder
// ---------------------------------------------------------------------------

bool RenderRaytracing::_build_merged_mm_blas(
		RID p_mm_rid,
		RID p_mm_gpu_buffer,
		void *p_mesh_surface,
		uint32_t p_mm_count,
		uint32_t p_surface_index,
		uint32_t p_lod,
		uint32_t p_surface_counter,
		RD::ComputeListID p_compute_list,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list,
		RTSurfaceData *r_surf_data) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, p_lod);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, p_lod);
	bool indexed = index_buffer.is_valid() && index_count > 0;
	uint32_t prim_count = indexed ? (index_count / 3) : (vertex_count / 3);

	// Skip compressed meshes: their positions are UNORM16x4, not float3.
	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	if (surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
		return false;
	}

	if (prim_count == 0 || vertex_count == 0) {
		return false;
	}
	static const uint32_t MM_MERGED_BLAS_MAX_TRIANGLES = (uint32_t)GLOBAL_GET("rendering/pathtracing/multimesh_merged_blas_max_triangles");
	// Vertex replication is independent of index LOD. Choose the native batch
	// representation from full-detail geometry so camera LOD switches cannot
	// change the TLAS topology or exceed its allocation budget.
	const uint32_t base_index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);
	const uint32_t base_primitives = base_index_count ? base_index_count / 3 : vertex_count / 3;
	if ((uint64_t)p_mm_count * base_primitives > MM_MERGED_BLAS_MAX_TRIANGLES) {
		return false;
	}

	// Side table -> pool: dense direct index keyed by RID local_index, with a
	// validator stamp to detect RID local-index recycling for a different MM.
	const uint32_t li = p_mm_rid.get_local_index();
	const uint32_t mm_validator = static_cast<uint32_t>(p_mm_rid.get_id() >> 32);
	if (li >= mm_handles.size()) {
		mm_handles.resize(li + 1);
	}
	MMSurfaceHandles &mm_entry = mm_handles[li];
	if (mm_entry.mm_validator != mm_validator) {
		// Local index has been recycled (or first-ever access). Release any slots
		// the previous owner left behind so we don't leak until TTL.
		RD *rd_local = RD::get_singleton();
		for (uint32_t s = 0; s < mm_entry.per_surface.size(); s++) {
			for (RID &h : mm_entry.per_surface[s]) {
				RTMergedMMEntry *old = merged_mm_pool.get_or_null(h);
				if (!old) {
					h = RID();
					continue;
				}
				if (old->blas.is_valid()) {
					rd_local->free_rid(old->blas);
				}
				if (old->merged_vtx_buffer.is_valid()) {
					rd_local->free_rid(old->merged_vtx_buffer);
				}
				if (old->merged_attr_buffer.is_valid()) {
					rd_local->free_rid(old->merged_attr_buffer);
				}
				if (old->replicated_idx_buffer.is_valid()) {
					rd_local->free_rid(old->replicated_idx_buffer);
				}
				merged_mm_pool.free(h);
				h = RID();
		}
		}
		mm_entry.per_surface.clear();
		mm_entry.mm_validator = mm_validator;
	}

	RTMergedMMEntry *entry_ptr = _access_merged_mm_slot(mm_entry.surface_handle(p_surface_index, p_lod));
	ERR_FAIL_NULL_V(entry_ptr, false);
	RTMergedMMEntry &entry = *entry_ptr;
	const uint32_t cache_key = (li << 8) | (p_surface_index & 0xFF); // Used only for resource naming below.

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	// Cache retention is independent of whether this geometry has been baked.
	entry.last_used_frame = current_frame;

	// Re-bake only after native instance data changes. This also distinguishes
	// multiple updates in one frame and does not depend on motion-vector mode.
	const uint64_t mm_revision = mesh_storage->multimesh_get_rt_revision(p_mm_rid);
	const bool instances_changed = (entry.cached_mm_revision != mm_revision);

	const RID source_mesh = mesh_storage->multimesh_get_mesh(p_mm_rid);
	const uint64_t source_mesh_revision = mesh_storage->mesh_get_rt_revision(source_mesh);
	bool structure_changed = (entry.cached_source_mesh != source_mesh ||
			entry.cached_source_mesh_revision != source_mesh_revision || entry.last_mm_count != p_mm_count ||
			entry.last_surface_counter != p_surface_counter);

	entry.indexed = indexed;

	if (structure_changed) {
		RD *rd = RD::get_singleton();
		if (entry.blas.is_valid()) {
			rd->free_rid(entry.blas);
			entry.blas = RID();
		}
		entry.blas_built_once = false;
		entry.cached_source_mesh = source_mesh;
		entry.cached_source_mesh_revision = source_mesh_revision;
		entry.last_mm_count = p_mm_count;
		entry.last_surface_counter = p_surface_counter;
	}

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;
	bool has_tbn = has_normal;

	// Layout of uncompressed vertex buffer: [float3 positions × V] + [packed TBN × V].
	// normal_stride = 8 when both normal+tangent present (two uint16x2 packed), 4 with normal only.
	uint32_t tbn_stride = 0;
	if (has_normal && has_tangent) {
		tbn_stride = 8; // 2 × uint32 (normal oct, tangent oct+sign)
	} else if (has_normal) {
		tbn_stride = 4; // 1 × uint32 (normal oct only)
	}
	// Byte offset of the TBN block in the source vertex buffer.
	uint32_t src_tbn_byte_offset = vertex_count * 12; // after all float3 positions

	// Merged vertex buffer: [float3 pos × N*V] + [packed TBN × N*V] (if TBN present).
	uint32_t merged_vtx_bytes = p_mm_count * vertex_count * 12 + (has_tbn ? p_mm_count * vertex_count * tbn_stride : 0);

	// Attribute buffer: attribute_stride bytes × V, replicated N times.
	RTSurfaceData meta_sd;
	_fill_surface_geometry_data(p_mesh_surface, p_lod, false, &meta_sd);
	uint32_t attrib_stride = meta_sd.geometry.attribute_stride;
	bool has_attr = attrib_stride > 0;
	const uint32_t MIN_ATTR_BYTES = 16;
	uint32_t merged_attr_bytes = has_attr ? (p_mm_count * vertex_count * attrib_stride) : MIN_ATTR_BYTES;

	RD *rd = RD::get_singleton();
	BitField<RD::BufferCreationBits> gpu_buf_flags =
			RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;

	// --- Grow / allocate merged vertex buffer ---
	if (!entry.merged_vtx_buffer.is_valid() || entry.vtx_capacity_bytes < merged_vtx_bytes) {
		if (entry.merged_vtx_buffer.is_valid()) {
			rd->free_rid(entry.merged_vtx_buffer);
			entry.merged_vtx_buffer = RID();
		}
		entry.vtx_capacity_bytes = merged_vtx_bytes;
		entry.merged_vtx_buffer = rd->vertex_buffer_create(merged_vtx_bytes, {}, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_vtx_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_vtx_buffer, "RT MM merged vtx [" + itos(cache_key) + "]");
		entry.blas_built_once = false;
	}

	// --- Grow / allocate merged attribute buffer ---
	if (!entry.merged_attr_buffer.is_valid() || entry.attr_capacity_bytes < merged_attr_bytes) {
		if (entry.merged_attr_buffer.is_valid()) {
			rd->free_rid(entry.merged_attr_buffer);
			entry.merged_attr_buffer = RID();
		}
		entry.attr_capacity_bytes = merged_attr_bytes;
		entry.merged_attr_buffer = rd->storage_buffer_create(merged_attr_bytes, {}, 0, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_attr_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_attr_buffer, "RT MM merged attr [" + itos(cache_key) + "]");
	}

	// --- Grow / allocate replicated index buffer ---
	if (indexed) {
		uint32_t needed_idx = p_mm_count * index_count;
		if (!entry.replicated_idx_buffer.is_valid() || entry.idx_capacity < needed_idx) {
			if (entry.replicated_idx_buffer.is_valid()) {
				rd->free_rid(entry.replicated_idx_buffer);
				entry.replicated_idx_buffer = RID();
			}
			entry.idx_capacity = needed_idx;
			entry.replicated_idx_buffer = rd->index_buffer_create(
					needed_idx, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, gpu_buf_flags);
			ERR_FAIL_COND_V(!entry.replicated_idx_buffer.is_valid(), false);
			rd->set_resource_name(entry.replicated_idx_buffer, "RT MM replicated idx [" + itos(cache_key) + "]");
			entry.blas_built_once = false;
		}
	}

	RID src_attr_buf;
	if (has_attr) {
		src_attr_buf = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
		ERR_FAIL_COND_V(!src_attr_buf.is_valid(), false);
	} else {
		src_attr_buf = entry.merged_attr_buffer;
	}

	MergeShader::Mode merge_mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;

	// Pre-validate source vertex buffer before opening the compute list.
	RID vtx_buf = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	ERR_FAIL_COND_V(!vtx_buf.is_valid(), false);
	uint64_t vtx_bda = rd->buffer_get_device_address(vtx_buf);

	const bool needs_rebake = instances_changed || !entry.blas_built_once;

	// --- Single merged dispatch: bake vertices + TBN + attributes + (optional) indices ---
	if (needs_rebake) {
		Vector<RD::Uniform> uniforms;
		{
			auto push_buf = [&](uint32_t binding, RID buf) {
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
				u.binding = binding;
				u.append_id(buf);
				uniforms.push_back(u);
			};
			push_buf(0, entry.merged_vtx_buffer);
			push_buf(1, p_mm_gpu_buffer);
			push_buf(2, entry.merged_attr_buffer);
			push_buf(3, src_attr_buf);
			if (indexed) {
				push_buf(4, entry.replicated_idx_buffer);
			}
		}
		RID merge_uniform_set = rd->uniform_set_create(uniforms, mm_merge_shader.version_shader[merge_mode], 0, /*p_linear_pool=*/true);
		ERR_FAIL_COND_V(!merge_uniform_set.is_valid(), false);
		uint32_t mm_stride = mesh_storage->multimesh_get_stride(p_mm_rid);
		uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(p_mm_rid);
		uint32_t tbn_stride_words = tbn_stride / 4;
		// In the merged vertex buffer the TBN block starts after all N*V float3 positions.
		uint32_t dst_tbn_base_words = p_mm_count * vertex_count * 3;

		struct MergePC {
			uint32_t src_vtx_lo, src_vtx_hi;
			// MODE_INDEXED only -- present in struct (uploaded only when indexed):
			uint32_t src_idx_lo, src_idx_hi;
			uint32_t index_count, src_is_16bit;
			// Common tail:
			uint32_t vertex_count, instance_count;
			uint32_t pos_stride_words;
			uint32_t src_tbn_base_words;
			uint32_t src_tbn_stride_words;
			uint32_t dst_tbn_base_words;
			uint32_t mm_stride, mm_offset;
			uint32_t has_tbn;
			uint32_t attr_stride_words;
		} pc;

		pc.src_vtx_lo = uint32_t(vtx_bda);
		pc.src_vtx_hi = uint32_t(vtx_bda >> 32);

		if (indexed) {
			uint64_t src_idx_bda = rd->buffer_get_device_address(index_buffer);
			pc.src_idx_lo = uint32_t(src_idx_bda);
			pc.src_idx_hi = uint32_t(src_idx_bda >> 32);
			pc.index_count = index_count;
			pc.src_is_16bit = (vertex_count <= 65536) ? 1u : 0u;
		}

		pc.vertex_count = vertex_count;
		pc.instance_count = p_mm_count;
		pc.pos_stride_words = 3; // always float3 (uncompressed check at top)
		pc.src_tbn_base_words = src_tbn_byte_offset / 4;
		pc.src_tbn_stride_words = tbn_stride_words;
		pc.dst_tbn_base_words = dst_tbn_base_words;
		pc.mm_stride = mm_stride;
		pc.mm_offset = mm_cur_offset;
		pc.has_tbn = has_tbn ? 1u : 0u;
		pc.attr_stride_words = has_attr ? (attrib_stride / 4) : 0u;

		MergeShader::Mode mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;
		rd->compute_list_bind_compute_pipeline(p_compute_list, mm_merge_shader.pipeline[mode]);
		rd->compute_list_bind_uniform_set(p_compute_list, merge_uniform_set, 0);
		if (indexed) {
			rd->compute_list_set_push_constant(p_compute_list, &pc, sizeof(MergePC));
		} else {
			// Re-pack the non-indexed PC so that the common tail follows
			// src_vtx_lo/hi without the index gap.
			struct MergePCNonIndexed {
				uint32_t src_vtx_lo, src_vtx_hi;
				uint32_t vertex_count, instance_count;
				uint32_t pos_stride_words;
				uint32_t src_tbn_base_words;
				uint32_t src_tbn_stride_words;
				uint32_t dst_tbn_base_words;
				uint32_t mm_stride, mm_offset;
				uint32_t has_tbn;
				uint32_t attr_stride_words;
			} pc_ni;
			pc_ni.src_vtx_lo = pc.src_vtx_lo;
			pc_ni.src_vtx_hi = pc.src_vtx_hi;
			pc_ni.vertex_count = pc.vertex_count;
			pc_ni.instance_count = pc.instance_count;
			pc_ni.pos_stride_words = pc.pos_stride_words;
			pc_ni.src_tbn_base_words = pc.src_tbn_base_words;
			pc_ni.src_tbn_stride_words = pc.src_tbn_stride_words;
			pc_ni.dst_tbn_base_words = pc.dst_tbn_base_words;
			pc_ni.mm_stride = pc.mm_stride;
			pc_ni.mm_offset = pc.mm_offset;
			pc_ni.has_tbn = pc.has_tbn;
			pc_ni.attr_stride_words = pc.attr_stride_words;
			rd->compute_list_set_push_constant(p_compute_list, &pc_ni, sizeof(MergePCNonIndexed));
		}

		// Single thread count: every thread processes one vertex (idx < N*V)
		// and -- for MODE_INDEXED -- one output index (idx < N*I) using disjoint
		// destination buffers, so no in-shader barrier is required.
		uint32_t thread_count = p_mm_count * vertex_count;
		if (indexed) {
			thread_count = MAX(thread_count, p_mm_count * index_count);
		}
		rd->compute_list_dispatch_threads(p_compute_list, thread_count, 1, 1);

		// Drop the RID now that the dispatch is recorded; the underlying Vulkan
		// descriptor set lives in the linear pool until frame end.
		rd->free_rid(merge_uniform_set);
	}

	// --- Build or refit the merged BLAS (uses merged_vtx_buffer for positions) ---
	if (!entry.blas.is_valid()) {
		RD::AccelerationStructureGeometry as_geom;
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = entry.merged_vtx_buffer;
		as_geom.geometry.triangles.vertex_stride = 12; // float3, positions section only
		as_geom.geometry.triangles.vertex_count = p_mm_count * vertex_count;
		as_geom.geometry.triangles.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;

		if (indexed) {
			as_geom.geometry.triangles.index_buffer = entry.replicated_idx_buffer;
			as_geom.geometry.triangles.index_count = p_mm_count * index_count;
		}

		BitField<RD::AccelerationStructureFlagBits> as_flags =
				RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT |
				RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		entry.blas = rd->blas_create({ &as_geom, 1 }, as_flags);
		ERR_FAIL_COND_V(!entry.blas.is_valid(), false);
		rd->set_resource_name(entry.blas, "RT MM merged BLAS [" + itos(cache_key) + "]");
	}

	if (needs_rebake) {
		if (!entry.blas_built_once) {
			r_dirty_blas_list.push_back(entry.blas);
			entry.blas_built_once = true;
		} else {
			r_dirty_blas_update_list.push_back(entry.blas);
		}
		entry.cached_mm_revision = mm_revision;
	}

	// --- Populate r_surf_data from the metadata already computed above, then override merged buffer addresses.
	*r_surf_data = meta_sd;
	r_surf_data->blas = entry.blas;
	r_surf_data->is_compressed = false;
	r_surf_data->aabb_transform = Transform3D();

	RT_GeometryData &geom = r_surf_data->geometry;

	// Point vertex address at merged buffer (positions + TBN all baked world-space).
	geom.vertex_buffer_address = rd->buffer_get_device_address(entry.merged_vtx_buffer);
	geom.vertex_count = p_mm_count * vertex_count;
	geom.position_stride = 12; // float3, uncompressed
	geom.flags &= ~RT_GEOM_FLAG_COMPRESSED;

	// TBN section starts after all positions in the merged vertex buffer.
	if (has_tbn) {
		uint32_t tbn_base = p_mm_count * vertex_count * 12;
		geom.normal_byte_offset = tbn_base;
		geom.normal_stride = tbn_stride;
		if (has_tangent) {
			geom.tangent_byte_offset = tbn_base; // tangent is at +4 from normal within the same stride pair
			geom.tangent_stride = tbn_stride;
		} else {
			geom.tangent_byte_offset = RT_OFFSET_NONE;
			geom.tangent_stride = 0;
		}
	}

	// Point attribute address at fully replicated attribute buffer.
	if (has_attr && entry.merged_attr_buffer.is_valid()) {
		geom.attribute_buffer_address = rd->buffer_get_device_address(entry.merged_attr_buffer);
	}

	// Point index address at the replicated (uint32) index buffer.
	if (indexed && entry.replicated_idx_buffer.is_valid()) {
		geom.index_buffer_address = rd->buffer_get_device_address(entry.replicated_idx_buffer);
		geom.index_format = RT_INDEX_FORMAT_UINT32; // always uint32 in replicated buffer
		geom.primitive_count = p_mm_count * prim_count;
	} else {
		geom.index_buffer_address = 0;
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = p_mm_count * prim_count;
	}

	return true;
}

// ---------------------------------------------------------------------------
// TLAS creation (main entry point per frame)
// ---------------------------------------------------------------------------



uint32_t RenderRaytracing::_select_surface_lod(const void *p_instance_ptr, void *p_surface, const RenderDataRD *p_render_data) const {
	const auto *p_instance = static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(p_instance_ptr);
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const auto *scene = p_render_data->scene_data;
	if (scene->screen_mesh_lod_threshold <= 0.0 || !mesh_storage->mesh_surface_has_lod(p_surface)) {
		return 0;
	}
	float distance = 1.0;
	if (!scene->cam_orthogonal) {
		Vector3 minimum = p_instance->transformed_aabb.position;
		Vector3 maximum = minimum + p_instance->transformed_aabb.size;
		Vector3 camera = scene->main_cam_transform.origin;
		distance = Vector3().max(minimum - camera).max(camera - maximum).length();
	}
	uint32_t indices = 0;
	return mesh_storage->mesh_surface_get_lod(p_surface, p_instance->lod_model_scale * p_instance->lod_bias,
			distance * scene->lod_distance_multiplier, scene->screen_mesh_lod_threshold, indices);
}

RTViewportState *RenderRaytracing::build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags) {
	static const bool profiling = OS::get_singleton()->has_environment("GODOT_RTX_PROFILE");
	const uint64_t profile_start = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
	if (!p_render_data || !p_render_data->rt_instances) {
		return nullptr;
	}

	RTViewportState *state = _get_or_create_viewport_state(p_render_data);
	if (!state) {
		return nullptr;
	}

	// Retain native merged buffers referenced by this viewport before TTL eviction.
	if (state->scene_cached) {
		for (RID handle : state->cached_merged_handles) {
			RTMergedMMEntry *entry = merged_mm_pool.get_or_null(handle);
			if (entry) {
				entry->last_used_frame = RSG::rasterizer->get_frame_number();
			}
		}
	}
	if (state->scene_cached) {
		for (const RTSurfaceKey &key : state->cached_surface_keys) {
			RTCacheEntry *entry = surface_cache.getptr(key);
			if (entry) {
				entry->last_used_frame = RSG::rasterizer->get_frame_number();
			}
		}
	}
	prepare_frame();

	// Builds bundle if needed; live_ready_mask drives TLAS inclusion below.
	SceneShaderRaytracing *rt_shader_singleton = SceneShaderRaytracing::get_singleton();
	rt_shader_singleton->ensure_pipeline_bundle(p_rt_flags);
	update_decals(p_render_data, state, p_rt_flags);
	const uint64_t pipeline_generation = rt_shader_singleton->get_pipeline_generation(p_rt_flags);

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const uint64_t geometry_revision = mesh_storage->get_rt_geometry_revision();
	const uint64_t material_revision = material_storage->get_rt_material_revision();
	LocalVector<RID> dirty_blas_list;
	LocalVector<RID> dirty_blas_update_list;

#ifdef TOOLS_ENABLED
	uint32_t tlas_instance_count = 0;
	uint32_t tlas_primitive_count = 0;
	uint32_t rt_blas_builds = 0;
	uint32_t rt_blas_refits = 0;
	uint32_t rt_triangles_built = 0;
	uint32_t rt_triangles_refit = 0;
	const bool collect_render_info = (p_render_data->render_info != nullptr);
	// Report only after the scene tables and TLAS match the published pipeline.
	// Shader registration during traversal may grow the required group set.
	auto report_scene_readiness = [&]() {
		if (!collect_render_info) {
			return;
		}
		uint32_t required, ready, failed;
		rt_shader_singleton->get_pipeline_group_counts(p_rt_flags, required, ready, failed);
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_SHADER_GROUPS] += required;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_SHADER_GROUPS_READY] += ready;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_SHADER_GROUPS_FAILED] += failed;
		const bool tables_ready = state->cached_instance_count == 0 || (state->geometry_buffer.is_valid() && state->material_buffer.is_valid());
		const bool scene_ready = required > 0 && required == ready && failed == 0 &&
				state->cached_shader_pending_omissions == 0 && state->tlas.is_valid() && tables_ready &&
				state->cached_pipeline_generation == rt_shader_singleton->get_pipeline_generation(p_rt_flags) &&
				state->cached_geometry_revision == mesh_storage->get_rt_geometry_revision() &&
				state->cached_material_revision == material_storage->get_rt_material_revision();
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_SCENE_READY] += scene_ready ? 1 : 0;
	};
#endif

	const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
	scene_inputs.resize(rt_instances.size());
	bool static_scene = true;
	// Global changes are only a signal to validate this viewport's actual native
	// resource dependencies. Editor grid geometry is not a world dependency.
	bool geometry_unchanged = state->cached_geometry_revision == geometry_revision;
	if (state->scene_cached && !geometry_unchanged) {
		geometry_unchanged = true;
		for (const KeyValue<RID, uint64_t> &cached : state->cached_mesh_revisions) {
			if (mesh_storage->mesh_get_rt_revision(cached.key) != cached.value) {
				geometry_unchanged = false;
				break;
			}
		}
		if (geometry_unchanged) {
			for (const KeyValue<RID, uint64_t> &cached : state->cached_multimesh_revisions) {
				if (mesh_storage->multimesh_get_rt_revision(cached.key) != cached.value) {
					geometry_unchanged = false;
					break;
				}
			}
		}
	}
	bool reuse_scene = state->scene_cached && state->cached_rt_flags == p_rt_flags &&
			geometry_unchanged &&
			state->cached_pipeline_generation == rt_shader_singleton->get_pipeline_generation(p_rt_flags) &&
			state->cached_scene_inputs.size() == scene_inputs.size();
	bool lods_unchanged = true;
	struct LODChange { const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surface; uint32_t lod; RTSurfaceData *data = nullptr; uint32_t selection = 0; };
	LocalVector<LODChange> lod_changes;
	bool ordered_inputs_match = state->cached_scene_inputs.size() == scene_inputs.size();
	for (uint32_t i = 0; i < scene_inputs.size(); i++) {
		const auto *inst = static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);

		RTSceneInput &input = scene_inputs[i];
		input.instance = uint64_t(uintptr_t(inst));
		input.revision = inst ? inst->rt_revision : 0;
		if (ordered_inputs_match && (input.instance != state->cached_scene_inputs[i].instance || input.revision != state->cached_scene_inputs[i].revision)) {
			ordered_inputs_match = false;
		}
		if (inst && (inst->rt_procedural || inst->mesh_instance.is_valid() || !inst->data ||
				(inst->data->base_type != RSE::INSTANCE_MESH && inst->data->base_type != RSE::INSTANCE_MULTIMESH))) {
			static_scene = false;
		}
	}
	reuse_scene = reuse_scene && static_scene;
	if (reuse_scene && !ordered_inputs_match) {
		// Scenario deletion swaps its final entry into the vacated slot. That
		// ordering change does not change cached TLAS IDs or their matching tables.
		reuse_scene = state->cached_input_revisions.size() == scene_inputs.size();
		for (const RTSceneInput &input : scene_inputs) {
			const uint64_t *revision = state->cached_input_revisions.getptr(input.instance);
			if (!revision || *revision != input.revision) {
				reuse_scene = false;
				break;
			}
		}
	}
	if (reuse_scene) {
		for (const RTSurfaceKey &key : state->cached_surface_keys) {
			RTCacheEntry *entry = surface_cache.getptr(key);
			if (!entry || !entry->ptr || !RD::get_singleton()->acceleration_structure_is_valid(entry->ptr->blas)) {
				reuse_scene = false;
				break;
			}
		}
	}
	if (reuse_scene) {
		const bool material_updates = state->cached_material_revision != material_storage->get_rt_material_revision();
		for (KeyValue<RID, RTSceneMaterial> &cached : state->cached_materials) {
			// Native feedback buffers rotate independently of scene/material revisions.
			// A changed index must reach the GPU table before reusing this scene.
			const uint32_t feedback_index = _get_material_feedback_index(cached.key);
			if (feedback_index != cached.value.data.material_feedback_index) {
				reuse_scene = false;
				break;
			}
			if (material_updates) {
				uint64_t revision = material_storage->material_get_rt_invalidation_counter(cached.key);
				if (revision != cached.value.revision) {
					RTMaterialData *updated = process_material(cached.key, revision);
					if (updated->rt_sbt_offset != cached.value.sbt || memcmp(&updated->data, &cached.value.data, sizeof(RT_MaterialData)) != 0) {
						reuse_scene = false;
						break;
					}
					cached.value.revision = revision;
				}
			}
			get_material_cache_entry(get_rid_index(cached.key))->last_used_frame = RSG::rasterizer->get_frame_number();
		}
	}
	if (reuse_scene) {
		for (RID handle : state->cached_merged_handles) {
			RTMergedMMEntry *entry = merged_mm_pool.get_or_null(handle);
			if (!entry || !entry->blas.is_valid()) {
				reuse_scene = false;
				break;
			}
			entry->last_used_frame = RSG::rasterizer->get_frame_number();
		}
	}
	if (reuse_scene) {
		// Dense native LOD selections contain only surfaces emitted into this
		// viewport's TLAS. No per-frame hash lookup or alpha-surface traversal.
		for (uint32_t i = 0; i < state->lod_surfaces.size(); i++) {
			const auto &selection = state->lod_surfaces[i];
			const auto *surface = static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(selection.surface);
			const uint32_t lod = _select_surface_lod(surface->owner, surface->surface, p_render_data);
			if (lod != selection.lod) { lods_unchanged = false; lod_changes.push_back({ surface, lod, nullptr, i }); }
		}
	}
	if (reuse_scene && !lods_unchanged) {
		// Preserve every unaffected native instance and material/motion table.
		state->scene_cached = false;
		state->cached_pipeline_generation = UINT64_MAX;
		for (LODChange &change : lod_changes) {
			if (!state->surface_ranges.has(change.surface)) { continue; }
			change.data = process_surface(change.surface, change.surface->surface,
					mesh_storage->mesh_surface_get_rt_invalidation_counter(change.surface->surface), change.lod, change.surface->owner->transform, dirty_blas_list);
			ERR_FAIL_COND_V(!change.data || !change.data->blas.is_valid(), nullptr);
		}
		struct ChangedRange {
			uint32_t first;
			LocalVector<RD::AccelerationStructureInstance> instances;
			LocalVector<RT_GeometryData> geometry;
		};
		LocalVector<ChangedRange> updates;
		RD::ComputeListID compute = RD::get_singleton()->compute_list_begin();
		for (const LODChange &change : lod_changes) {
			RTViewportState::SurfaceRange *range = state->surface_ranges.getptr(change.surface);
			if (!range) { continue; }
			const auto *inst = change.surface->owner;
			RTSurfaceData merged;
			RTSurfaceData *data = change.data;
			const bool multimesh = inst->data->base_type == RSE::INSTANCE_MULTIMESH;
			RID mm = multimesh ? inst->data->base : RID();
			if (range->merged) {
				const bool built = _build_merged_mm_blas(mm, mesh_storage->multimesh_get_gpu_buffer(mm), change.surface->surface,
						mesh_storage->multimesh_get_instances_to_draw(mm), change.surface->surface_index, change.lod,
						mesh_storage->mesh_surface_get_rt_invalidation_counter(change.surface->surface), compute, dirty_blas_list, dirty_blas_update_list, &merged);
				if (!built) { RD::get_singleton()->compute_list_end(); ERR_FAIL_V(nullptr); }
				data = &merged;
				range->merged_handle = merged_mm_active_this_frame[merged_mm_active_this_frame.size() - 1];
			}
			ChangedRange update; update.first = range->first;
			update.instances.resize(range->count); update.geometry.resize(range->count);
			const float *mm_data = multimesh && !range->merged ? mesh_storage->multimesh_get_local_data_ptr(mm) : nullptr;
			const uint32_t stride = mm_data ? mesh_storage->multimesh_get_stride(mm) : 0;
			const uint32_t offset = mm_data ? mesh_storage->multimesh_get_current_instance_offset(mm) : 0;
			for (uint32_t i = 0; i < range->count; i++) {
				Transform3D transform = inst->transform;
				RT_GeometryData &geometry = update.geometry[i]; geometry = data->geometry;
				geometry.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset); geometry.layer_mask = inst->layer_mask;
				if (mm_data) {
					const float *d = mm_data + (offset + i) * stride; Transform3D local;
					for (uint32_t row = 0; row < 3; row++) {
						for (uint32_t col = 0; col < 3; col++) { local.basis.rows[row][col] = d[row * 4 + col]; }
						local.origin[row] = d[row * 4 + 3];
					}
					transform = transform * local;
					if (mesh_storage->multimesh_uses_custom_data(mm)) {
						for (uint32_t channel = 0; channel < 4; channel++) { geometry.instance_custom[channel] = d[stride - 4 + channel]; }
					}
				}
				if (data->is_compressed) { transform = transform * data->aabb_transform; }
				RD::AccelerationStructureInstance &instance = update.instances[i];
				instance.id = range->first + i; instance.transform = transform; instance.blas = data->blas;
				instance.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(range->flags); instance.mask = 0xff;
				instance.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | uint64_t(range->sbt));
			}
			state->cached_primitive_count -= range->primitives * range->count;
			range->primitives = data->geometry.primitive_count;
			state->cached_primitive_count += range->primitives * range->count;
			updates.push_back(std::move(update));
		}
		RD::get_singleton()->compute_list_end();
		for (RID blas : dirty_blas_list) { ERR_FAIL_COND_V(RD::get_singleton()->blas_build(blas) != OK, nullptr); }
		for (RID blas : dirty_blas_update_list) { ERR_FAIL_COND_V(RD::get_singleton()->blas_update(blas) != OK, nullptr); }
		LocalVector<RD::AccelerationStructureInstanceRange> ranges;
		for (ChangedRange &update : updates) {
			ERR_FAIL_COND_V(RD::get_singleton()->buffer_update(state->geometry_buffer, update.first * sizeof(RT_GeometryData), update.geometry.size() * sizeof(RT_GeometryData), update.geometry.ptr()) != OK, nullptr);
			ranges.push_back({ update.first, { update.instances.ptr(), update.instances.size() } });
		}
		if (ranges.size()) { ERR_FAIL_COND_V(RD::get_singleton()->tlas_update_instances(state->tlas, ranges) != OK, nullptr); }
		for (const LODChange &change : lod_changes) { state->lod_surfaces[change.selection].lod = change.lod; state->surface_ranges[change.surface].lod = change.lod; }
		state->cached_surface_keys.clear(); state->cached_merged_handles.clear();
		HashSet<RTSurfaceKey, RTSurfaceKeyHasher> active;
		for (const KeyValue<const void *, RTViewportState::SurfaceRange> &cached : state->surface_ranges) {
			const auto *surface = static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(cached.key);
			RID mesh = surface->owner->data->base;
			if (surface->owner->data->base_type == RSE::INSTANCE_MULTIMESH) { mesh = mesh_storage->multimesh_get_mesh(mesh); }
			RTSurfaceKey key{ mesh, surface->surface_index, cached.value.lod };
			if (!active.has(key)) { active.insert(key); state->cached_surface_keys.push_back(key); }
			if (cached.value.merged_handle.is_valid()) { state->cached_merged_handles.push_back(cached.value.merged_handle); }
		}
		state->scene_cached = true;
		state->cached_pipeline_generation = pipeline_generation;
#ifdef TOOLS_ENABLED
		if (collect_render_info) {
			p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS] += dirty_blas_list.size();
			p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_REFITS] += dirty_blas_update_list.size();
		}
#endif
		if (profiling) { print_line(vformat("RTX LOD RANGES: surfaces=%d ranges=%d CPU=%.2f ms", lod_changes.size(), ranges.size(), (OS::get_singleton()->get_ticks_usec() - profile_start) / 1000.0)); }
	}
	if (reuse_scene) {
		if (!ordered_inputs_match) {
			state->cached_scene_inputs = scene_inputs;
		}
		state->cached_geometry_revision = geometry_revision;
		state->cached_material_revision = material_revision;
		merged_mm_active_this_frame = state->cached_merged_handles;
#ifdef TOOLS_ENABLED
		if (collect_render_info) {
			p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += state->cached_instance_count;
			p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += state->cached_primitive_count;
			p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES] += state->cached_instance_count;
		}
#endif
		rt_shader_singleton->finalize_custom_shaders();
#ifdef TOOLS_ENABLED
		report_scene_readiness();
#endif
		if (profiling && RSG::rasterizer->get_frame_number() % 60 == 0) {
			print_line(vformat("RTX PROFILE: early_reuse=1 inputs=%d instances=%d CPU=%.2f ms", scene_inputs.size(), state->cached_instance_count, (OS::get_singleton()->get_ticks_usec() - profile_start) / 1000.0));
		}
		return state;
	}

	if (profiling) {
		print_line(vformat("RTX CACHE MISS: flags=%x cached_flags=%x inputs=%d cached_inputs=%d geometry_revision=%d cached_geometry_revision=%d pipeline_generation=%d cached_pipeline_generation=%d", p_rt_flags, state->cached_rt_flags, scene_inputs.size(), state->cached_scene_inputs.size(), geometry_revision, state->cached_geometry_revision, pipeline_generation, state->cached_pipeline_generation));
	}
	// -----------------------------------------------------------------------
	// Phase 1: CPU / buffer-update work
	// -----------------------------------------------------------------------
	state->scene_cached = false;
	state->cached_pipeline_generation = UINT64_MAX;
	state->surface_ranges.clear();
	uint32_t shader_pending_omissions = 0;
	struct PendingMMSurface {
		RID mm_rid;
		RID mm_gpu_buffer;
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf;
		void *mesh_surface;
		uint32_t mm_count;
		uint32_t surface_index;
		uint32_t surface_counter;
		uint32_t lod;
		Transform3D instance_transform;
		Transform3D prev_instance_transform;
		bool transform_moved;
		RTMaterialData *mat_data;
		uint32_t inst_flags;
		uint32_t instance_uniforms_ofs;
		uint32_t layer_mask;
		RTSurfaceData *surface_data;
	};
	LocalVector<PendingMMSurface> pending_mm_surfaces;

	for (uint32_t i = 0; i < (uint32_t)rt_instances.size(); i++) {
		const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
				static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);
		if (!inst || !inst->data) {
			continue;
		}
		const Transform3D &instance_transform = inst->transform;

		// Determine previous-frame transform for motion vectors.
		const Transform3D &prev_instance_transform =
				(inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::TELEPORTED)
				? inst->transform
				: inst->prev_transform;

		// Handle procedural RT instances (intersection shaders).
		if (inst->rt_procedural) {
			SceneShaderRaytracing *rt_shader = SceneShaderRaytracing::get_singleton();
			RTProceduralState *ps = inst->rt_procedural;

			// Intersection code comes from ShaderMaterial on material_override.
			if (!inst->data || !inst->data->material_override.is_valid()) {
				continue;
			}
			RID proc_material_rid = inst->data->material_override;
			uint32_t shader_id = material_storage->material_get_shader_id(proc_material_rid);
			if (shader_id == 0) {
				continue;
			}

			uint32_t hg_index = rt_shader->register_procedural_shader(shader_id, proc_material_rid);
			if (hg_index == 0) {
				continue;
			}
			if (!rt_shader->is_hg_ready_in_bundle(hg_index, p_rt_flags)) {
				shader_pending_omissions++;
				continue;
			}

			if (ps->dirty) {
#ifdef TOOLS_ENABLED
				uint32_t pre_proc_build_size = dirty_blas_list.size();
#endif
				update_procedural_blas(ps, dirty_blas_list);
				ps->dirty = false;
#ifdef TOOLS_ENABLED
				if (collect_render_info) {
					rt_blas_builds += dirty_blas_list.size() - pre_proc_build_size;
				}
#endif
			}

			if (ps->blas.is_valid()) {
				blass.push_back(ps->blas);
				blas_transforms.push_back(instance_transform);
				sbt_offsets.push_back(hg_index);

				RT_GeometryData geom = {};
				geom.flags = RT_GEOM_FLAG_PROCEDURAL;
				geom.vertex_buffer_address = ps->gpu_buffer_address;
				geom.aabb_size_x = (float)ps->culling_aabb.size.x;
				geom.aabb_size_y = (float)ps->culling_aabb.size.y;
				geom.aabb_size_z = (float)ps->culling_aabb.size.z;
				geom.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
				geom.layer_mask = inst->layer_mask;
				geometry_data.push_back(geom);

				if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_instance_transform, motion.prev_object_to_world);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}

				// Material for procedural geometry (already validated above).
				uint64_t proc_mat_counter = material_storage->material_get_rt_invalidation_counter(proc_material_rid);
				RTMaterialData *proc_mat_data = process_material(proc_material_rid, proc_mat_counter);
				material_data.push_back(proc_mat_data->data);

				// Procedural instances disable triangle culling and are opaque.
				uint32_t inst_flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT |
						RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				instance_flags.push_back(inst_flags);
				instance_masks.push_back(0xFF);
			}
			continue;
		}

		// MultiMesh: resolve materials and warm data cache now.
		// Compute dispatches and TLAS assembly are deferred to Phase 2.
		if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
			RID mm_rid = inst->data->base;

			if (mesh_storage->multimesh_get_transform_format(mm_rid) != RSE::MULTIMESH_TRANSFORM_3D) {
				continue;
			}

			uint32_t mm_count = mesh_storage->multimesh_get_instances_to_draw(mm_rid);
			if (mm_count == 0) {
				continue;
			}

			if (mesh_storage->multimesh_uses_indirect(mm_rid)) {
				static_scene = false;
			}
			RID mm_gpu_buffer = mesh_storage->multimesh_get_gpu_buffer(mm_rid);
			// Populate data cache now — first access triggers GPU readback, safe here.
			mesh_storage->multimesh_get_local_data_ptr(mm_rid);

			bool transform_moved = (inst->transform_status ==
					RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED);

			const RenderForwardClustered::GeometryInstanceSurfaceDataCache *mm_surf = inst->surface_caches;
			while (mm_surf) {
				if (mm_surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) {
					mm_surf = mm_surf->next;
					continue;
				}

				void *mesh_surface = mm_surf->surface;
				uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);

				RID material_rid;
				if (mm_surf->owner->data->material_override.is_valid()) {
					material_rid = mm_surf->owner->data->material_override;
				} else if (mm_surf->surface_index < mm_surf->owner->data->surface_materials.size() &&
						mm_surf->owner->data->surface_materials[mm_surf->surface_index].is_valid()) {
					material_rid = mm_surf->owner->data->surface_materials[mm_surf->surface_index];
				} else {
					RID mesh_rid = mesh_storage->multimesh_get_mesh(mm_rid);
					if (mesh_rid.is_valid() && mesh_storage->owns_mesh(mesh_rid)) {
						material_rid = mesh_storage->mesh_surface_get_material(mesh_rid, mm_surf->surface_index);
					}
				}

				uint64_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
				RTMaterialData *mat_data = process_material(material_rid, material_counter);

				if (mat_data->rt_sbt_offset > 0 &&
						!rt_shader_singleton->is_hg_ready_in_bundle(mat_data->rt_sbt_offset, p_rt_flags)) {
					shader_pending_omissions++;
					mm_surf = mm_surf->next;
					continue;
				}

				uint32_t inst_flags = 0;
				if (mm_surf->shader) {
					switch (mm_surf->shader->rt_cull_mode()) {
						case RSE::CULL_MODE_DISABLED:
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
							break;
						case RSE::CULL_MODE_FRONT:
							break;
						case RSE::CULL_MODE_BACK:
						default:
							inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
							break;
					}
				} else {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
				}
				if (mat_data->rt_sbt_offset > 0) {
					const SceneShaderRaytracing::CustomShaderEntry *cse =
							rt_shader_singleton->get_custom_shader_entry(mat_data->rt_sbt_offset);
					if (!cse || (!cse->uses_alpha_clip && cse->alpha_texture_buffer_offset == UINT32_MAX)) {
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					}
				} else {
					bool is_alpha = mm_surf->shader &&
							(mm_surf->shader->uses_alpha_clip || mm_surf->shader->uses_blend_alpha || mm_surf->shader->uses_alpha);
					if (!is_alpha) {
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					}
				}

				PendingMMSurface pending;
				pending.mm_rid = mm_rid;
				pending.mm_gpu_buffer = mm_gpu_buffer;
				pending.mm_surf = mm_surf;
				pending.mesh_surface = mesh_surface;
				pending.mm_count = mm_count;
				pending.surface_index = mm_surf->surface_index;
				pending.surface_counter = surface_counter;
				pending.lod = _select_surface_lod(inst, mesh_surface, p_render_data);
				pending.instance_transform = instance_transform;
				pending.prev_instance_transform = prev_instance_transform;
				pending.transform_moved = transform_moved;
				pending.mat_data = mat_data;
				pending.inst_flags = inst_flags;
				pending.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
				pending.layer_mask = inst->layer_mask;
				// Material/geometry uploads must precede the merge compute list.
				pending.surface_data = process_surface(mm_surf, mesh_surface, surface_counter, pending.lod, instance_transform, dirty_blas_list);
				pending_mm_surfaces.push_back(pending);

				mm_surf = mm_surf->next;
			}
			continue;
		}

		// Walk the surface cache linked list.
		const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf = inst->surface_caches;
		bool instance_static = inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::NONE;
		while (surf) {
			// Skip surfaces routed to the raster alpha overlay
			if (surf->rt_pass_flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_PASS_ALPHA) {
				surf = surf->next;
				continue;
			}

			void *mesh_surface = surf->surface;
			uint32_t surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mesh_surface);

#ifdef TOOLS_ENABLED
			uint32_t pre_build_size = dirty_blas_list.size();
			uint32_t pre_refit_size = dirty_blas_update_list.size();
#endif

			// MeshInstance skinning/blend shapes provide a deformed vertex buffer.
			RTSurfaceData *surf_data = nullptr;
			if (inst->mesh_instance.is_valid()) {
				RID curr_vb = mesh_storage->mesh_instance_get_vertex_buffer(inst->mesh_instance, surf->surface_index);
				if (curr_vb.is_valid()) {
					RTDeformedGeometrySource src;
					src.current_vb = curr_vb;
					src.prev_vb = mesh_storage->mesh_instance_get_prev_vertex_buffer(inst->mesh_instance, surf->surface_index);
					src.change_stamp = mesh_storage->mesh_instance_get_last_change(inst->mesh_instance, surf->surface_index);
					uint64_t mi_id = inst->mesh_instance.get_id();
					uint32_t mi_index = static_cast<uint32_t>(mi_id & 0xFFFFFFFFULL);
					src.cache_version = static_cast<uint32_t>(mi_id >> 32);
					src.cache_key = (static_cast<uint64_t>(mi_index) << 16) | (surf->surface_index & 0xFFFFu);
					src.surface_counter = surface_counter;
					surf_data = process_deformed_surface(surf, mesh_surface, src, _select_surface_lod(inst, mesh_surface, p_render_data), dirty_blas_list, dirty_blas_update_list);
				}
			}
			if (!surf_data) {
				surf_data = process_surface(surf, mesh_surface, surface_counter, _select_surface_lod(inst, mesh_surface, p_render_data), instance_transform, dirty_blas_list);
			}
			if (!surf_data || !surf_data->blas.is_valid()) {
				surf = surf->next;
				continue;
			}

			// Resolve material before TLAS so we can skip surfaces whose HG is not live yet (override > surface > mesh).
			RID material_rid;
			if (surf->owner->data->material_override.is_valid()) {
				material_rid = surf->owner->data->material_override;
			} else if (surf->surface_index < surf->owner->data->surface_materials.size() &&
					surf->owner->data->surface_materials[surf->surface_index].is_valid()) {
				material_rid = surf->owner->data->surface_materials[surf->surface_index];
			} else {
				RID mesh_rid = surf->owner->data->base;
				if (mesh_rid.is_valid() && mesh_storage->owns_mesh(mesh_rid)) {
					material_rid = mesh_storage->mesh_surface_get_material(mesh_rid, surf->surface_index);
				}
			}

			uint64_t material_counter = material_storage->material_get_rt_invalidation_counter(material_rid);
			RTMaterialData *mat_data = process_material(material_rid, material_counter);

			if (mat_data->rt_sbt_offset > 0 &&
					!rt_shader_singleton->is_hg_ready_in_bundle(mat_data->rt_sbt_offset, p_rt_flags)) {
				shader_pending_omissions++;
				surf = surf->next;
				continue;
			}

			// Compute or reuse cached final transform (instance * aabb_transform for compressed meshes).
			Transform3D final_transform;
			if (instance_static && surf->cached_final_transform_valid) {
				final_transform = surf->cached_final_transform;
			} else {
				final_transform = instance_transform;
				if (surf_data->is_compressed) {
					final_transform = instance_transform * surf_data->aabb_transform;
				}
				surf->cached_final_transform = final_transform;
				surf->cached_final_transform_valid = true;
			}
			const uint32_t surface_first = blass.size();
			blas_transforms.push_back(final_transform);

			blass.push_back(surf_data->blas);
			geometry_data.push_back(surf_data->geometry);
			geometry_data[geometry_data.size() - 1].instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
			geometry_data[geometry_data.size() - 1].layer_mask = inst->layer_mask;

			if (inst->transform_status == RenderForwardClustered::GeometryInstanceForwardClustered::TransformStatus::MOVED) {
				motion_indices.push_back((int32_t)motion_transforms.size());
				RT_InstanceMotionData motion = {};
				Transform3D prev_final = prev_instance_transform;
				if (surf_data->is_compressed) {
					prev_final = prev_instance_transform * surf_data->aabb_transform;
				}
				RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_world);
				motion_transforms.push_back(motion);
			} else {
				motion_indices.push_back(-1);
			}

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count++;
				uint32_t prim_count = surf_data->geometry.primitive_count;
				tlas_primitive_count += prim_count;
				uint32_t build_delta = dirty_blas_list.size() - pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif

			sbt_offsets.push_back(mat_data->rt_sbt_offset);
			material_data.push_back(mat_data->data);

			// Determine per-instance TLAS flags from material properties.
			uint32_t inst_flags = 0;
			if (surf->shader) {
				switch (surf->shader->rt_cull_mode()) {
					case RSE::CULL_MODE_DISABLED:
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
						break;
					case RSE::CULL_MODE_FRONT:
						break;
					case RSE::CULL_MODE_BACK:
					default:
						inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
						break;
				}
			} else {
				inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
			}

			if (mat_data->rt_sbt_offset > 0) {
				// Custom shader: enable any-hit if it uses alpha clip or has a hint_albedo texture.
				const SceneShaderRaytracing::CustomShaderEntry *cse =
						rt_shader_singleton->get_custom_shader_entry(mat_data->rt_sbt_offset);
				if (!cse || (!cse->uses_alpha_clip && cse->alpha_texture_buffer_offset == UINT32_MAX)) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
			} else {
				// Standard material: FORCE_OPAQUE if no alpha usage.
				bool is_alpha = surf->shader && (surf->shader->uses_alpha_clip || surf->shader->uses_blend_alpha || surf->shader->uses_alpha);
				if (!is_alpha) {
					inst_flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
				}
			}
			instance_flags.push_back(inst_flags);
			instance_masks.push_back(0xFF);
			state->surface_ranges.insert(surf, { surface_first, 1, inst_flags, mat_data->rt_sbt_offset, surf_data->geometry.primitive_count, false, RID() });

			surf = surf->next;
		}
	}

	// -----------------------------------------------------------------------
	// Phase 2: GPU compute — merged MultiMesh BLAS dispatches.
	// -----------------------------------------------------------------------
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	for (const PendingMMSurface &pending : pending_mm_surfaces) {
		const uint32_t surface_first = blass.size();
#ifdef TOOLS_ENABLED
		uint32_t mm_pre_build_size = dirty_blas_list.size();
		uint32_t mm_pre_refit_size = dirty_blas_update_list.size();
#endif
		RTSurfaceData merged_sd;
		// Custom vertex code needs the original per-instance model matrix and data.
		bool use_merged = pending.mat_data->rt_sbt_offset == 0 && pending.mm_gpu_buffer.is_valid() &&
				_build_merged_mm_blas(pending.mm_rid, pending.mm_gpu_buffer, pending.mesh_surface,
						pending.mm_count, pending.surface_index, pending.lod, pending.surface_counter,
						compute_list, dirty_blas_list, dirty_blas_update_list, &merged_sd);

		if (use_merged) {
			blass.push_back(merged_sd.blas);
			blas_transforms.push_back(pending.instance_transform);
			geometry_data.push_back(merged_sd.geometry);
			geometry_data[geometry_data.size() - 1].instance_uniforms_ofs = pending.instance_uniforms_ofs;
			geometry_data[geometry_data.size() - 1].layer_mask = pending.layer_mask;
			sbt_offsets.push_back(pending.mat_data->rt_sbt_offset);
			material_data.push_back(pending.mat_data->data);
			motion_indices.push_back(-1);
			instance_flags.push_back(pending.inst_flags);
			instance_masks.push_back(0xFF);
			state->surface_ranges.insert(pending.mm_surf, { surface_first, 1, pending.inst_flags, pending.mat_data->rt_sbt_offset, merged_sd.geometry.primitive_count, true, merged_mm_active_this_frame[merged_mm_active_this_frame.size() - 1] });
#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count++;
				uint32_t prim_count = merged_sd.geometry.primitive_count;
				tlas_primitive_count += prim_count;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		} else {
			// Per-instance TLAS entries share the immutable mesh BLAS.
			const float *mm_data = mesh_storage->multimesh_get_local_data_ptr(pending.mm_rid);
			if (!mm_data) {
				continue;
			}

			const uint32_t mm_stride = mesh_storage->multimesh_get_stride(pending.mm_rid);
			const uint32_t mm_cur_offset = mesh_storage->multimesh_get_current_instance_offset(pending.mm_rid);

			RTSurfaceData *surf_data = pending.surface_data;
			if (!surf_data || !surf_data->blas.is_valid()) {
				continue;
			}

			for (uint32_t mi = 0; mi < pending.mm_count; mi++) {
				const float *d = mm_data + (mm_cur_offset + mi) * mm_stride;
				Transform3D mm_xform;
				mm_xform.basis.rows[0][0] = d[0];
				mm_xform.basis.rows[0][1] = d[1];
				mm_xform.basis.rows[0][2] = d[2];
				mm_xform.origin.x = d[3];
				mm_xform.basis.rows[1][0] = d[4];
				mm_xform.basis.rows[1][1] = d[5];
				mm_xform.basis.rows[1][2] = d[6];
				mm_xform.origin.y = d[7];
				mm_xform.basis.rows[2][0] = d[8];
				mm_xform.basis.rows[2][1] = d[9];
				mm_xform.basis.rows[2][2] = d[10];
				mm_xform.origin.z = d[11];

				Transform3D final_transform = pending.instance_transform * mm_xform;
				if (surf_data->is_compressed) {
					final_transform = final_transform * surf_data->aabb_transform;
				}

				blass.push_back(surf_data->blas);
				blas_transforms.push_back(final_transform);
				geometry_data.push_back(surf_data->geometry);
				geometry_data[geometry_data.size() - 1].instance_uniforms_ofs = pending.instance_uniforms_ofs;
			geometry_data[geometry_data.size() - 1].layer_mask = pending.layer_mask;
				if (mesh_storage->multimesh_uses_custom_data(pending.mm_rid)) {
					for (uint32_t channel = 0; channel < 4; channel++) {
						geometry_data[geometry_data.size() - 1].instance_custom[channel] = d[mm_stride - 4 + channel];
					}
				}
				sbt_offsets.push_back(pending.mat_data->rt_sbt_offset);
				material_data.push_back(pending.mat_data->data);

				if (pending.transform_moved) {
					Transform3D prev_final = pending.prev_instance_transform * mm_xform;
					if (surf_data->is_compressed) {
						prev_final = prev_final * surf_data->aabb_transform;
					}
					motion_indices.push_back((int32_t)motion_transforms.size());
					RT_InstanceMotionData motion = {};
					RendererRD::MaterialStorage::store_transform_transposed_3x4(prev_final, motion.prev_object_to_world);
					motion_transforms.push_back(motion);
				} else {
					motion_indices.push_back(-1);
				}

				instance_flags.push_back(pending.inst_flags);
				instance_masks.push_back(0xFF);
			}

			state->surface_ranges.insert(pending.mm_surf, { surface_first, pending.mm_count, pending.inst_flags, pending.mat_data->rt_sbt_offset, surf_data->geometry.primitive_count, false, RID() });

#ifdef TOOLS_ENABLED
			if (collect_render_info) {
				tlas_instance_count += pending.mm_count;
				uint32_t prim_count = pending.surface_data->geometry.primitive_count;
				tlas_primitive_count += prim_count * pending.mm_count;
				uint32_t build_delta = dirty_blas_list.size() - mm_pre_build_size;
				uint32_t refit_delta = dirty_blas_update_list.size() - mm_pre_refit_size;
				rt_blas_builds += build_delta;
				rt_blas_refits += refit_delta;
				rt_triangles_built += prim_count * build_delta;
				rt_triangles_refit += prim_count * refit_delta;
			}
#endif
		}
	}

	// Phase 3: BLAS / TLAS build.
	const uint64_t profile_prepare_end = profiling ? OS::get_singleton()->get_ticks_usec() : 0;
#ifdef TOOLS_ENABLED
	if (collect_render_info) {
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += tlas_primitive_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES] += tlas_instance_count;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS] += rt_blas_builds;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_BLAS_REFITS] += rt_blas_refits;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_BUILT] += rt_triangles_built;
		p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_REFIT] += rt_triangles_refit;
	}
#endif

	SceneShaderRaytracing::get_singleton()->finalize_custom_shaders();
	const uint64_t profile_shader_end = profiling ? OS::get_singleton()->get_ticks_usec() : 0;

	RD::get_singleton()->compute_list_end();

	ERR_FAIL_COND_V(build_acceleration_structures(state, dirty_blas_list, dirty_blas_update_list) != OK, nullptr);
	finalize_buffers(state);
	// Record every input resource, including surfaces currently omitted from RT.
	// A MultiMesh depends on both its instance buffer and its shared source mesh.
	state->cached_mesh_revisions.clear();
	state->cached_multimesh_revisions.clear();
	for (uint32_t i = 0; i < rt_instances.size(); i++) {
		const auto *inst = static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(rt_instances[i]);
		if (!inst || !inst->data) {
			continue;
		}
		RID mesh;
		if (inst->data->base_type == RSE::INSTANCE_MESH) {
			mesh = inst->data->base;
		} else if (inst->data->base_type == RSE::INSTANCE_MULTIMESH) {
			const RID multimesh = inst->data->base;
			state->cached_multimesh_revisions.insert(multimesh, mesh_storage->multimesh_get_rt_revision(multimesh));
			mesh = mesh_storage->multimesh_get_mesh(multimesh);
		}
		if (mesh.is_valid()) {
			state->cached_mesh_revisions.insert(mesh, mesh_storage->mesh_get_rt_revision(mesh));
		}
	}
	state->cached_scene_inputs = scene_inputs;
	state->cached_input_revisions.clear();
	for (const RTSceneInput &input : scene_inputs) {
		state->cached_input_revisions.insert(input.instance, input.revision);
	}
	state->cached_materials = scene_materials;
	state->cached_geometry_revision = geometry_revision;
	state->cached_material_revision = material_revision;
	state->cached_pipeline_generation = pipeline_generation;
	state->cached_rt_flags = p_rt_flags;
	state->lod_surfaces.clear();
	for (KeyValue<const void *, RTViewportState::SurfaceRange> &cached : state->surface_ranges) {
		const auto *surface = static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(cached.key);
		cached.value.lod = _select_surface_lod(surface->owner, surface->surface, p_render_data);
		if (mesh_storage->mesh_surface_has_lod(surface->surface)) { state->lod_surfaces.push_back({ surface, cached.value.lod }); }
	}

	state->cached_surface_keys.clear();
	for (const RTSurfaceKey &key : surface_active_this_frame) {
		state->cached_surface_keys.push_back(key);
	}
	state->cached_merged_handles = merged_mm_active_this_frame;
	state->cached_instance_count = blass.size();
#ifdef TOOLS_ENABLED
	state->cached_primitive_count = tlas_primitive_count;
#endif
	state->scene_cached = static_scene;
	state->cached_shader_pending_omissions = shader_pending_omissions;
#ifdef TOOLS_ENABLED
	report_scene_readiness();
#endif
	if (profiling) {
		print_line(vformat("RTX PROFILE: instances=%d prep=%.2f ms shader_finalize=%.2f ms build_upload_cpu=%.2f ms BLASbuild=%d BLASrefit=%d cache_hits=%d cache_misses=%d", blass.size(), (profile_prepare_end - profile_start) / 1000.0, (profile_shader_end - profile_prepare_end) / 1000.0, (OS::get_singleton()->get_ticks_usec() - profile_shader_end) / 1000.0, dirty_blas_list.size(), dirty_blas_update_list.size(), cache_hits, cache_misses));
	}

	return state;
}

// ---------------------------------------------------------------------------
// Light gathering
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::gather_lights(const RenderDataRD *p_render_data, RT_LightData *r_light_data, uint32_t p_max_lights) {
	uint32_t rt_light_count = 0;

	if (!p_render_data || !p_render_data->lights) {
		return rt_light_count;
	}

	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	const Transform3D &cam_xform = p_render_data->scene_data->cam_transform;
	const Vector3 cam_pos = cam_xform.origin;
	const Vector3 cam_forward = -cam_xform.basis.get_column(2).normalized(); // -Z is forward in Godot.

	// Compute light energy matching rasterizer conventions
	auto compute_light_energy = [&](RID p_base, RSE::LightType p_type) -> float {
		float sign = ls->light_is_negative(p_base) ? -1.0f : 1.0f;
		float e = sign * ls->light_get_param(p_base, RSE::LIGHT_PARAM_ENERGY);
		if (owner->is_using_physical_light_units()) {
			e *= ls->light_get_param(p_base, RSE::LIGHT_PARAM_INTENSITY);
			if (p_type == RSE::LIGHT_OMNI) {
				e *= 1.0f / (Math::PI * 4.0f);
			} else if (p_type == RSE::LIGHT_SPOT) {
				e *= 1.0f / Math::PI;
			}
		} else {
			e *= Math::PI;
		}
		if (p_render_data->camera_attributes.is_valid()) {
			e *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}
		return e;
	};

	// Scoring helper: approximate power/solid-angle contribution.
	struct LightScore {
		RID light_instance;
		float score;
	};

	LocalVector<LightScore> positional_lights;

	// Lights behind the camera are deprioritized. The penalty ramps in linearly
	// over BEHIND_FRUSTUM_BUFFER_M so nearby behind-camera lights aren't punished harshly.
	// At or in front of the camera plane: no penalty. Beyond the buffer: full penalty.
	static const float BEHIND_FRUSTUM_PENALTY = 0.05f;
	static const float BEHIND_FRUSTUM_BUFFER_M = 20.0f;

	// Helper: score a positional light and add to candidates.
	auto score_positional_light = [&](RID light_instance) {
		RID base = ls->light_instance_get_base_light(light_instance);
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		Vector3 light_pos = xform.origin;
		Vector3 to_light = light_pos - cam_pos;
		float dist_sq = MAX(to_light.dot(to_light), 0.01f);
		Color color = ls->light_get_color(base);
		float energy = ls->light_get_param(base, RSE::LIGHT_PARAM_ENERGY);
		float lum = color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f;

		// Range factor: strongly prefer lights with a large influence radius.
		// Lights with no range limit (range == 0) are treated as infinite.
		float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
		float range_factor = (range > 0.0f) ? (range * range) : 1.0e12f;

		// Signed depth of the light along the camera forward axis (world units).
		// Positive = in front, negative = behind. Ramp penalty in over the buffer zone.
		float forward_depth = to_light.dot(cam_forward);
		float t = CLAMP((forward_depth + BEHIND_FRUSTUM_BUFFER_M) / BEHIND_FRUSTUM_BUFFER_M, 0.0f, 1.0f);
		float frustum_factor = Math::lerp(BEHIND_FRUSTUM_PENALTY, 1.0f, t);

		float score = (energy * lum * range_factor * frustum_factor) / dist_sq;

		LightScore ls_entry = {};
		ls_entry.light_instance = light_instance;
		ls_entry.score = score;
		positional_lights.push_back(ls_entry);
	};

	// Directional lights from the frustum-culled list (they're global, always included).
	const PagedArray<RID> &lights = *p_render_data->lights;
	for (uint32_t li = 0; li < (uint32_t)lights.size(); li++) {
		RID light_instance = lights[li];
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		if (type != RSE::LIGHT_DIRECTIONAL) {
			continue;
		}
		if (rt_light_count >= p_max_lights) {
			break;
		}
		RT_LightData &ld = r_light_data[rt_light_count];
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		Vector3 dir = -xform.basis.get_column(2).normalized();
		ld.position[0] = dir.x;
		ld.position[1] = dir.y;
		ld.position[2] = dir.z;
		ld.type = RT_LIGHT_TYPE_DIRECTIONAL;
		Color linear_col = ls->light_get_color(base).srgb_to_linear();
		float energy = compute_light_energy(base, RSE::LIGHT_DIRECTIONAL);
		ld.emission[0] = linear_col.r * energy;
		ld.emission[1] = linear_col.g * energy;
		ld.emission[2] = linear_col.b * energy;
		ld.radius = Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE) * 0.5f); // Half-angle in radians.
		ld.attenuation = 0.0f; // No distance attenuation.
		ld.inv_max_range = -1.0f; // Infinite range.
		ld.max_range_squared = 0.0f;
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR);
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		ld.flags = ls->light_has_shadow(base) ? RT_LIGHT_SHADOW_ENABLED : 0u;
		ld.inv_spot_attenuation = 0.0f;
		ld.cos_spot_angle = 0.0f;
		ld.spot_direction[0] = 0.0f;
		ld.spot_direction[1] = 0.0f;
		ld.spot_direction[2] = 0.0f;
		rt_light_count++;
	}

	// Positional lights from the AABB-culled RT list (superset of frustum).
	if (p_render_data->rt_lights) {
		const PagedArray<RID> &rt_lights = *p_render_data->rt_lights;
		for (uint32_t li = 0; li < (uint32_t)rt_lights.size(); li++) {
			score_positional_light(rt_lights[li]);
		}
	}

	// Sort all positional lights by score descending.
	struct LightScoreComparator {
		bool operator()(const LightScore &a, const LightScore &b) const {
			return a.score > b.score;
		}
	};
	positional_lights.sort_custom<LightScoreComparator>();

	// Fill remaining slots with top positional lights.
	for (uint32_t i = 0; i < positional_lights.size() && rt_light_count < p_max_lights; i++) {
		RID light_instance = positional_lights[i].light_instance;
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		RT_LightData &ld = r_light_data[rt_light_count];
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		ld.position[0] = xform.origin.x;
		ld.position[1] = xform.origin.y;
		ld.position[2] = xform.origin.z;
		ld.type = (type == RSE::LIGHT_SPOT) ? RT_LIGHT_TYPE_SPOT : RT_LIGHT_TYPE_OMNI;

		Color linear_col = ls->light_get_color(base).srgb_to_linear();
		float energy = compute_light_energy(base, type);
		ld.emission[0] = linear_col.r * energy;
		ld.emission[1] = linear_col.g * energy;
		ld.emission[2] = linear_col.b * energy;
		ld.radius = ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE);
		ld.attenuation = ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
		float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
		if (range > 0.0f) {
			ld.inv_max_range = 1.0f / range;
			ld.max_range_squared = range * range;
		} else {
			ld.inv_max_range = -1.0f;
			ld.max_range_squared = 0.0f;
		}
		ld.specular_amount = ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f; // Matches rasterizer convention (light_storage.cpp), normalizes 0.5 default to 1.0.
		ld.indirect_energy = ls->light_get_param(base, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		ld.flags = ls->light_has_shadow(base) ? RT_LIGHT_SHADOW_ENABLED : 0u;

		if (type == RSE::LIGHT_SPOT) {
			ld.inv_spot_attenuation = 1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
			float spot_angle_deg = ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE);
			ld.cos_spot_angle = Math::cos(Math::deg_to_rad(spot_angle_deg));
			Vector3 spot_dir = -xform.basis.get_column(2).normalized();
			ld.spot_direction[0] = spot_dir.x;
			ld.spot_direction[1] = spot_dir.y;
			ld.spot_direction[2] = spot_dir.z;
		} else {
			ld.inv_spot_attenuation = 0.0f;
			ld.cos_spot_angle = 0.0f;
			ld.spot_direction[0] = 0.0f;
			ld.spot_direction[1] = 0.0f;
			ld.spot_direction[2] = 0.0f;
		}
		rt_light_count++;
	}

	return rt_light_count;
}

// ---------------------------------------------------------------------------
// Uniform set update
// ---------------------------------------------------------------------------

RID RenderRaytracing::update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags) {
	ERR_FAIL_NULL_V(p_state, RID());

	Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		if (p_render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			rb_data = p_render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
	}

	if (rb_data.is_null()) {
		return RID();
	}

	RenderSceneBuffersRD *rb = p_render_data->render_buffers.ptr();

	// SET 0 indices must match raytracing_common_inc.glsl / scene_raytracing_raygen.glsl / samplers includes.
	Vector<RD::Uniform> uniforms;

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		rt_ensure_textures(rb);
		u.append_id(rt_get_texture(rb));
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		ERR_FAIL_COND_V(p_state->tlas == RID(), RID());
		u.append_id(p_state->tlas);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(owner->scene_state.uniform_buffers[0]);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->geometry_buffer.is_valid()) {
			u.append_id(p_state->geometry_buffer);
		} else {
			// Use a default buffer if no geometry
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 4: Per-instance motion index buffer (int32 per TLAS instance, -1 = no motion).
	{
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_index_buffer.is_valid()) {
			u.append_id(p_state->motion_index_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Motion transforms past sampler block growth reservation (bindings 28-31).
	{
		RD::Uniform u;
		u.binding = 32;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_transform_buffer.is_valid()) {
			u.append_id(p_state->motion_transform_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 5: Material buffer.
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->material_buffer.is_valid()) {
			u.append_id(p_state->material_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 6: Raytracing params + unjittered VP matrices.
	{
		struct {
			//TODO: I see no reason why we need to pass this as a float array
			float params[16];
			float prev_vp_unjittered[16];
			float curr_vp_unjittered[16];
		} rt_ubo = {};
		static_assert(sizeof(rt_ubo) == 48 * sizeof(float));

		if (p_render_data && p_render_data->environment.is_valid()) {
			RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();
			RID env = p_render_data->environment;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_VIS_MODE] = (float)env_storage->environment_get_pathtracing_debug_mode(env);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_SAMPLE_COUNT] = (float)env_storage->environment_get_pathtracing_samples_per_pixel(env);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_MAX_BOUNCES] = (float)env_storage->environment_get_pathtracing_max_bounces(env);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_DENOISER] = (float)(int)env_storage->environment_get_pathtracing_denoiser(env);
		}

		// rt_params layout (see RaytracingParamIndex enum):
		// [0] = VIS_MODE, [1] = SAMPLE_COUNT, [2] = MAX_BOUNCES,
		// [3] = DLSS_RR_ENABLED, [14] = LIGHT_COUNT, [15] = FRAME_INDEX
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FRAME_INDEX] = float(p_state->frame_counter++);

		// Unjittered VP for motion vectors (matches raster convention).
		{
			Projection correction;
			correction.set_depth_correction(true);

			Projection prev_vp = (correction * p_render_data->scene_data->prev_cam_projection) * Projection(p_render_data->scene_data->prev_cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(prev_vp, rt_ubo.prev_vp_unjittered);

			Projection curr_vp = (correction * p_render_data->scene_data->cam_projection) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(curr_vp, rt_ubo.curr_vp_unjittered);
		}

		// --- Light gathering ---
		uint32_t rt_light_count = 0;
		RT_LightData rt_light_data[RT_LIGHTS_MAX] = {};

		rt_light_count = gather_lights(p_render_data, rt_light_data, RT_LIGHTS_MAX);

		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_LIGHT_COUNT] = float(rt_light_count);

		// Upload light buffer.
		{
			uint32_t buf_size = RT_LIGHTS_MAX * sizeof(RT_LightData);
			if (!p_state->light_buffer.is_valid()) {
				p_state->light_buffer = RD::get_singleton()->storage_buffer_create(buf_size);
				RD::get_singleton()->set_resource_name(p_state->light_buffer, "RT Light Buffer");
			}
			RD::get_singleton()->buffer_update(p_state->light_buffer, 0, buf_size, rt_light_data);
		}

		if (!p_state->params_buffer.is_valid()) {
			p_state->params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(rt_ubo));
			RD::get_singleton()->set_resource_name(p_state->params_buffer, "RT Params Buffer");
		}
		RD::get_singleton()->buffer_update(p_state->params_buffer, 0, sizeof(rt_ubo), &rt_ubo);

		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(p_state->params_buffer);
		uniforms.push_back(u);
	}

	// Binding 7: Sky radiance octahedral map (for pathtracing sky sampling).
	{
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		const bool use_octmap_array = owner->is_using_radiance_octmap_array();
		RID radiance_texture;

		if (p_render_data && p_render_data->environment.is_valid()) {
			RID sky_rid = owner->environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				radiance_texture = use_octmap_array
						? owner->sky.sky_get_radiance_texture_rd(sky_rid)
						: owner->sky.sky_get_radiance_2d_texture_rd(sky_rid);
			}
		}

		// Fall back to a default whose type matches the shader's declared binding.
		if (!radiance_texture.is_valid()) {
			radiance_texture = texture_storage->texture_rd_get_default(use_octmap_array
							? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK
							: RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		}

		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}

	// Binding 8: Sampler for radiance texture (linear filtering with mipmaps and clamp).
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
				RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		uniforms.push_back(u);
	}

	// Bindings 9-12: DLSS Ray Reconstruction output buffers (only in DLSS RR shader variant).
	bool dlss_rr_enabled = dlss_rr_has_buffers(rb);
	if (dlss_rr_enabled) {
		// Binding 9: DLSS RR Diffuse Albedo
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_diffuse_albedo(rb));
			uniforms.push_back(u);
		}

		// Binding 10: DLSS RR Specular Albedo
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_specular_albedo(rb));
			uniforms.push_back(u);
		}

		// Binding 11: DLSS RR Normal + Roughness
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_normal_roughness(rb));
			uniforms.push_back(u);
		}

		// Binding 12: DLSS RR Specular Hit Distance
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_specular_hit_dist(rb));
			uniforms.push_back(u);
		}
	}

	// Binding 13: Light buffer (SSBO).
	{
		RD::Uniform u;
		u.binding = 13;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->light_buffer.is_valid()) {
			u.append_id(p_state->light_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 14: Global shader uniforms SSBO.
	{
		RD::Uniform u;
		u.binding = 14;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID buf = RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer();
		if (buf.is_valid()) {
			u.append_id(buf);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 15: RT depth output (R32F storage image for writing depth from closest_hit/miss).
	{
		RD::Uniform u;
		u.binding = 15;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rt_get_depth_texture(rb));
		uniforms.push_back(u);
	}

	// Bindings 16-27: Material samplers (12 filter/repeat combinations for custom shaders).
	RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default().append_uniforms(uniforms, 16);

	// Binding 28: Velocity output (RG16F). Past the 16-27 sampler range.
	{
		rb->ensure_velocity();
		RD::Uniform u;
		u.binding = 28;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rb->get_velocity_buffer(false));
		uniforms.push_back(u);
	}

	{
		RD::Uniform u; u.binding = 33; u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		u.append_id(p_state->decal_tlas); uniforms.push_back(u);
	}
	{
		RD::Uniform u; u.binding = 34; u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(p_state->decal_buffer); uniforms.push_back(u);
	}
#ifdef MODULE_TEXTURE_STREAMING_ENABLED
	if (texture_streaming_enabled) {
		RD::Uniform u;
		u.binding = 35;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID feedback_buffer = TextureStreaming::get_singleton()->feedback_buffer_get_uniform_rid();
		if (feedback_buffer.is_null()) {
			// All material indices are UINT32_MAX while feedback is unavailable.
			feedback_buffer = RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer();
		}
		u.append_id(feedback_buffer);
		uniforms.push_back(u);
	}
#endif
	RID shader_rd = shader ? shader->get_pipeline_shader_rd(p_rt_flags) : RID();

	RID result;
	if (shader_rd.is_valid()) {
		// Release the previous frame's scene uniform set now (before allocating
		// this frame's). Disposal is deferred by RenderingDevice until the frame
		// that used it has finished on the GPU, so this is safe and prevents the
		// uniform_set_owner pool from leaking one RID per accumulated frame.
		//
		// The set references render-buffer resources, so RenderingDevice may have
		// already auto-freed it via its dependency cascade (e.g. a texture/buffer
		// it referenced was recreated). RID::is_valid() only checks for non-null,
		// so we must query uniform_set_is_valid() to avoid a double free.
		if (p_state->scene_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->scene_uniform_set)) {
			RD::get_singleton()->free_rid(p_state->scene_uniform_set);
		}
		p_state->scene_uniform_set = RID();

		result = RD::get_singleton()->uniform_set_create(
				uniforms,
				shader_rd,
				RenderForwardClustered::SCENE_UNIFORM_SET,
				/*p_linear_pool=*/true);
		p_state->scene_uniform_set = result;

		// === SET 1: Bindless textures ===
		if (bindless_block && bindless_block->is_initialized()) {
			bindless_block->finalize(shader_rd, 1);
			bindless_uniform_set = bindless_block->get_uniform_set();
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// Trace-time buffer dependencies
// ---------------------------------------------------------------------------

void RenderRaytracing::register_raytracing_buffer_dependencies(RD::RaytracingListID p_list) {
	RD *rd = RD::get_singleton();

	// The hit shader reads vertex / attribute / index data via BDA stored in
	// RT_GeometryData. The draw graph cannot infer those reads from the BDA, so
	// every per-frame-written buffer that the trace will touch via BDA must be
	// declared here -- otherwise the producing copy/dispatch may not be visible
	// to the trace. (Static mesh VB/AB/IB are uploaded once via the transfer
	// worker and don't need per-frame barriers.)

	// Mat UBO pool buffer dependency.
	if (mat_ubo_pool_buffer.is_valid()) {
		rd->raytracing_list_add_buffer_dependency(p_list, mat_ubo_pool_buffer, /*p_writable=*/false);
	}
	// Dedicated material uniforms are also read through device addresses. Track
	// their writes and lifetime just like the pool; the graph cannot infer BDA reads.
	for (uint32_t i = 0; i < material_chunks.size(); i++) {
		if (!material_chunks[i]) {
			continue;
		}
		for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
			RTMaterialData *material = material_chunks[i][j].ptr;
			if (material && material->uniform_buffer.is_valid()) {
				rd->raytracing_list_add_buffer_dependency(p_list, material->uniform_buffer, false);
			}
		}
	}

	// Deformed surface buffer dependencies.
	for (uint32_t i = 0; i < deformed_active_this_frame.size(); i++) {
		RTDeformedCacheEntry *e = deformed_pool.get_or_null(deformed_active_this_frame[i]);
		if (!e) {
			continue;
		}
		if (e->owned_vb_full.is_valid()) {
			rd->raytracing_list_add_buffer_dependency(p_list, e->owned_vb_full, /*p_writable=*/false);
		}
		if (e->prev_pos_vb.is_valid()) {
			rd->raytracing_list_add_buffer_dependency(p_list, e->prev_pos_vb, /*p_writable=*/false);
		}
	}

	// Merged MultiMesh buffer dependencies.
	for (uint32_t i = 0; i < merged_mm_active_this_frame.size(); i++) {
		RTMergedMMEntry *e = merged_mm_pool.get_or_null(merged_mm_active_this_frame[i]);
		if (!e) {
			continue;
		}
		if (e->merged_vtx_buffer.is_valid()) {
			rd->raytracing_list_add_buffer_dependency(p_list, e->merged_vtx_buffer, /*p_writable=*/false);
		}
		if (e->merged_attr_buffer.is_valid()) {
			rd->raytracing_list_add_buffer_dependency(p_list, e->merged_attr_buffer, /*p_writable=*/false);
		}
		if (e->replicated_idx_buffer.is_valid()) {
			rd->raytracing_list_add_buffer_dependency(p_list, e->replicated_idx_buffer, /*p_writable=*/false);
		}
	}
}

// ---------------------------------------------------------------------------
// Output copy
// ---------------------------------------------------------------------------

void RenderRaytracing::copy_output_texture(const RenderDataRD *p_render_data) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	if (!rt_has_texture(rb.ptr())) {
		return;
	}

	// Copy raytracing output to main color buffer
	for (uint32_t v = 0; v < rb->get_view_count(); v++) {
		RID src = rt_get_texture(rb.ptr());
		RID dst = rb->get_internal_texture(v);
		owner->copy_effects->copy_to_rect(src, dst, Rect2i(0, 0, rb->get_internal_size().x, rb->get_internal_size().y), false, false, false, false, false, true);
	}
}

void RenderRaytracing::update_decals(const RenderDataRD *p_render_data, RTViewportState *p_state, uint32_t p_rt_flags) {
	auto *rd = RD::get_singleton();
	auto *textures = RendererRD::TextureStorage::get_singleton();
	auto *materials = RendererRD::MaterialStorage::get_singleton();
	auto *shaders = SceneShaderRaytracing::get_singleton();
	LocalVector<RID> ids;
	if (p_render_data->rt_decals) for (uint32_t i = 0; i < p_render_data->rt_decals->size(); i++) ids.push_back((*p_render_data->rt_decals)[i]);
	ids.sort();
	bool same_ids = ids.size() == p_state->cached_decals.size();
	for (uint32_t i = 0; same_ids && i < ids.size(); i++) same_ids = ids[i] == p_state->cached_decals[i];
	const uint64_t revision = textures->get_rt_decal_revision();
	const uint64_t material_revision = materials->get_rt_material_revision();
	const uint64_t generation = shaders->get_pipeline_generation(p_rt_flags);
	bool feedback_indices_unchanged = true;
	for (const KeyValue<RID, uint32_t> &cached : p_state->cached_decal_feedback_indices) {
		if (_get_material_feedback_index(cached.key) != cached.value) {
			feedback_indices_unchanged = false;
		}
	}
	if (feedback_indices_unchanged && same_ids && revision == p_state->cached_decal_revision && material_revision == p_state->cached_decal_material_revision && generation == p_state->cached_decal_pipeline_generation && (!p_state->decal_distance_fade || p_state->cached_decal_camera == p_render_data->scene_data->cam_transform.origin)) return;
	p_state->cached_decal_feedback_indices.clear();
	struct Projector { RID rid; RendererRD::TextureStorage::RTDecal data; };
	LocalVector<Projector> projectors;
	for (RID id : ids) { Projector p; p.rid = id; if (textures->decal_instance_get_rt_data(id, p.data)) projectors.push_back(p); }
	struct Order { bool operator()(const Projector &a, const Projector &b) const { return a.data.sorting_offset == b.data.sorting_offset ? a.rid < b.rid : a.data.sorting_offset < b.data.sorting_offset; } };
	projectors.sort_custom<Order>();
	if (decal_unit_blas.is_null()) {
		Vector<uint8_t> bytes; bytes.resize(24); const float bounds[6] = { -.5f, -.5f, -.5f, .5f, .5f, .5f }; memcpy(bytes.ptrw(), bounds, 24);
		decal_unit_buffer = rd->storage_buffer_create(24, bytes, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		RD::AccelerationStructureGeometry g; g.type = RD::AccelerationStructureGeometry::TYPE_AABBS;
		g.geometry.aabbs.buffer = decal_unit_buffer; g.geometry.aabbs.count = 1; g.geometry.aabbs.stride = 24;
		decal_unit_blas = rd->blas_create({ &g, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT); rd->blas_build(decal_unit_blas);
	}
	p_state->decal_distance_fade = false;
	p_state->cached_decal_camera = p_render_data->scene_data->cam_transform.origin;
	LocalVector<RT_DecalData> table; table.resize(projectors.size());
	LocalVector<RD::AccelerationStructureInstance> instances; instances.resize(projectors.size());
	auto store_matrix = [](const Transform3D &t, float *dst) { for (int col = 0; col < 4; col++) { Vector3 v = col == 3 ? t.origin : t.basis.get_column(col); for (int row = 0; row < 3; row++) dst[col * 4 + row] = v[row]; dst[col * 4 + 3] = col == 3 ? 1.f : 0.f; } };
	Vector<uint8_t> transforms; transforms.resize(projectors.size() * sizeof(Transform3D));
	for (uint32_t i = 0; i < projectors.size(); i++) {
		const auto &p = projectors[i].data; RT_DecalData &d = table[i]; d = {};
		Transform3D model = p.transform; model.basis.scale_local(p.size);
		store_matrix(model, d.model); store_matrix(model.affine_inverse(), d.inverse_model);
		memcpy(transforms.ptrw() + i * sizeof(Transform3D), &model, sizeof(model));
		for (int c = 0; c < 4; c++) { d.instance_custom[c] = p.instance_custom[c]; d.modulate[c] = p.modulate[c]; }
		d.fades[0] = p.upper_fade; d.fades[1] = p.lower_fade; d.fades[2] = p.normal_fade; d.fades[3] = 1.f;
		d.parameters[0] = p.albedo_mix; d.parameters[1] = p.emission_energy;
		if (p.distance_fade) { p_state->decal_distance_fade = true; const float distance = p.transform.origin.distance_to(p_render_data->scene_data->cam_transform.origin); d.fades[3] = 1.f - CLAMP((distance - p.distance_fade_begin) / p.distance_fade_length, 0.f, 1.f); }
		d.cull_mask = p.cull_mask; d.callable_index = UINT32_MAX;
		for (int c = 0; c < 4; c++) { d.textures[c] = UINT32_MAX; if (p.textures[c].is_valid()) { RID tex = textures->texture_get_rd_texture(p.textures[c], c == RSE::DECAL_TEXTURE_ALBEDO || c == RSE::DECAL_TEXTURE_EMISSION); if (tex.is_valid()) d.textures[c] = bindless_block->add_texture(tex); } }
		if (p.material.is_valid()) {
			RTMaterialData *m = process_material(p.material, materials->material_get_rt_invalidation_counter(p.material));
			d.material_feedback_index = m->data.material_feedback_index;
			p_state->cached_decal_feedback_indices.insert(p.material, d.material_feedback_index);
			const auto *entry = shaders->get_custom_shader_entry(m->rt_sbt_offset);
			ERR_FAIL_COND_MSG(!entry || !entry->is_decal, "Native Decal material requires render_mode decal.");
			if (shaders->is_hg_ready_in_bundle(m->rt_sbt_offset, p_rt_flags)) { d.callable_index = entry->callable_index; d.uniform_address = m->data.uniform_address; }
		}
		auto &instance = instances[i]; instance.id = i; instance.transform = model; instance.blas = decal_unit_blas; instance.mask = 0xff; instance.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32));
	}
	const uint32_t needed = MAX(1u, instances.size());
	bool rebuild = !same_ids || transforms.size() != p_state->cached_decal_transforms.size() || (transforms.size() && memcmp(transforms.ptr(), p_state->cached_decal_transforms.ptr(), transforms.size()) != 0);
	if (p_state->decal_tlas_capacity < needed) { if (p_state->decal_tlas.is_valid()) rd->free_rid(p_state->decal_tlas); p_state->decal_tlas_capacity = needed * 2; p_state->decal_tlas = rd->tlas_create(p_state->decal_tlas_capacity, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT); rebuild = true; }
	if (rebuild) rd->tlas_build(p_state->decal_tlas, instances);
	const uint32_t byte_count = 16 + table.size() * sizeof(RT_DecalData);
	Vector<uint8_t> bytes; bytes.resize(byte_count); memset(bytes.ptrw(), 0, byte_count); const uint32_t count = table.size(); memcpy(bytes.ptrw(), &count, 4); if (count) memcpy(bytes.ptrw() + 16, table.ptr(), table.size() * sizeof(RT_DecalData));
	if (p_state->decal_buffer_capacity < byte_count) { if (p_state->decal_buffer.is_valid()) rd->free_rid(p_state->decal_buffer); p_state->decal_buffer_capacity = byte_count * 2; p_state->decal_buffer = rd->storage_buffer_create(p_state->decal_buffer_capacity); }
	rd->buffer_update(p_state->decal_buffer, 0, byte_count, bytes.ptr());
	p_state->cached_decals = ids; p_state->cached_decal_transforms.resize(transforms.size()); if (transforms.size()) memcpy(p_state->cached_decal_transforms.ptr(), transforms.ptr(), transforms.size());
	p_state->cached_decal_revision = revision; p_state->cached_decal_material_revision = material_revision; p_state->cached_decal_pipeline_generation = generation;
}
