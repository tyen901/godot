layout(location = 0) callableDataEXT RTDecalCall decal_call;

// A zero-length query enumerates all AABBs containing the actual ray hit.
// Do not confirm candidates: confirmation would prune overlapping projectors.
void apply_rt_decals(HitData h, inout MaterialResult material_result) {
	if (decal_count == 0u) return;
	vec3 original_normal = material_result.normal;
	vec3 packed_normal = original_normal * (0.375 + 0.125 * (1.0 - material_result.roughness));
	uint previous = 0u;
	bool footprint_ready = false;
	vec3 dx = vec3(0), dy = vec3(0);
	for (;;) {
		uint next = decal_count;
		rayQueryEXT query;
		rayQueryInitializeEXT(query, decal_tlas, 0u, 0xffu, h.hit_pos, 0.0, vec3(1.0, 0.0, 0.0), 0.0);
		while (rayQueryProceedEXT(query)) {
			uint index = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
			if (index < previous || index >= next) continue;
			RTDecalData d = rt_decals[index];
			if ((d.cull_mask & geometries[h.geometry_idx].layer_mask) == 0u) continue;
			vec3 local = (d.inverse_model * vec4(h.hit_pos, 1.0)).xyz;
			if (any(greaterThan(abs(local), vec3(0.5)))) continue;
			next = index;
		}
		if (next == decal_count) break;
		previous = next + 1u;
		RTDecalData d = rt_decals[next];
		if (!footprint_ready) {
			GeometryData geom = geometries[h.geometry_idx];
			uint i0, i1, i2;
			get_triangle_indices(geom, i0, i1, i2);
			vec3 p0 = rt_triangle_position(geom, i0);
			vec3 e1 = mat3(gl_ObjectToWorldEXT) * (rt_triangle_position(geom, i1) - p0);
			vec3 e2 = mat3(gl_ObjectToWorldEXT) * (rt_triangle_position(geom, i2) - p0);
			vec3 plane_normal = cross(e1, e2);
			// Pixel footprint on the hit plane, transformed by the projector in its shader.
			mat4 view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0, 0, 0, 1)));
			vec4 clip = scene_data_block.data.projection_matrix * view_matrix * vec4(h.hit_pos, 1.0);
			vec2 ndc = clip.xy / clip.w;
			vec2 pixel = 2.0 / scene_data_block.data.viewport_size;
			vec3 derivatives[2];
			for (int axis = 0; axis < 2; axis++) {
				vec2 uv = ndc + (axis == 0 ? vec2(pixel.x, 0) : vec2(0, pixel.y));
				vec4 a = scene_data_block.data.inv_projection_matrix * vec4(uv, 0.5, 1.0);
				vec4 b = scene_data_block.data.inv_projection_matrix * vec4(uv, 1.0, 1.0);
				mat4 camera = inverse(view_matrix);
				vec3 origin = (camera * vec4(a.xyz / a.w, 1)).xyz;
				vec3 direction = mat3(camera) * (b.xyz / b.w - a.xyz / a.w);
				derivatives[axis] = origin + direction * (dot(h.hit_pos - origin, plane_normal) / dot(direction, plane_normal)) - h.hit_pos;
			}
			dx = derivatives[0]; dy = derivatives[1];
			footprint_ready = true;
		}
		if (d.callable_index != 0xffffffffu) {
			decal_call.index = next;
			decal_call.position = h.hit_pos;
			decal_call.original_normal = original_normal;
			decal_call.dx = dx; decal_call.dy = dy;
			decal_call.material_coverage = 0.0;
			decal_call.albedo_add = vec3(0);
			decal_call.albedo_retain = vec3(1.0);
			decal_call.normal_smoothness = packed_normal;
			decal_call.metalness = material_result.metalness;
			decal_call.transmission = material_result.transmission;
			decal_call.specular = material_result.specular;
			decal_call.emission = material_result.emissive;
			executeCallableEXT(d.callable_index, 0);
			float coverage = decal_call.material_coverage;
			material_result.albedo = decal_call.albedo_add + material_result.albedo * decal_call.albedo_retain;
			packed_normal = mix(packed_normal, decal_call.normal_smoothness, coverage);
			material_result.metalness = mix(material_result.metalness, decal_call.metalness, coverage);
			material_result.transmission = mix(material_result.transmission, decal_call.transmission, coverage);
			material_result.specular = mix(material_result.specular, decal_call.specular, coverage);
			material_result.emissive = mix(material_result.emissive, decal_call.emission, coverage);
		} else {
			vec3 local = (d.inverse_model * vec4(h.hit_pos, 1.0)).xyz;
			vec2 uv = local.xz + .5;
			vec2 ddx = (mat3(d.inverse_model) * dx).xz;
			vec2 ddy = (mat3(d.inverse_model) * dy).xz;
			float fade = pow(max(0.0, 1.0 - 2.0 * abs(local.y)), local.y > 0.0 ? d.fades.x : d.fades.y) * d.fades.w;
			vec3 projector_normal = normalize(d.model[1].xyz);
			if (d.fades.z > 0.0) fade *= smoothstep(d.fades.z, 1.0, dot(h.geometry_normal, projector_normal) * .5 + .5);
			if (d.textures.x != 0xffffffffu) {
				vec4 albedo = textureGrad(sampler2D(bindless_textures[nonuniformEXT(d.textures.x)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, ddx, ddy) * d.modulate;
				float coverage = albedo.a * fade;
				material_result.albedo = mix(material_result.albedo, albedo.rgb, coverage * d.parameters.x);
				vec3 normal = normalize(packed_normal);
				float smoothness = clamp(8.0 * length(packed_normal) - 3.0, 0.0, 1.0);
				if (d.textures.y != 0xffffffffu) {
					vec3 n = textureGrad(sampler2D(bindless_textures[nonuniformEXT(d.textures.y)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, ddx, ddy).xyz;
					n.xy = n.xy * vec2(2.0, -2.0) - vec2(1.0, -1.0);
					n.z = sqrt(max(0.0, 1.0 - dot(n.xy, n.xy)));
					normal = normalize(normalize(d.model[0].xyz) * n.x + projector_normal * n.z + normalize(d.model[2].xyz) * n.y);
				}
				if (d.textures.z != 0xffffffffu) {
					vec3 orm = textureGrad(sampler2D(bindless_textures[nonuniformEXT(d.textures.z)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, ddx, ddy).rgb;
					smoothness = 1.0 - orm.g;
					material_result.metalness = mix(material_result.metalness, orm.b, coverage);
				}
				packed_normal = mix(packed_normal, normal * (.375 + .125 * smoothness), coverage);
			}
			if (d.textures.w != 0xffffffffu) material_result.emissive += textureGrad(sampler2D(bindless_textures[nonuniformEXT(d.textures.w)], SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), uv, ddx, ddy).rgb * d.parameters.y * fade * scene_data_block.data.emissive_exposure_normalization;
		}
	}
	material_result.normal = normalize(packed_normal);
	material_result.roughness = 1.0 - clamp(8.0 * length(packed_normal) - 3.0, 0.0, 1.0);
}
