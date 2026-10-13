// Analytic texture derivatives shared by standard and custom ray-hit materials.
// GeometryBuffer, scene_data_block and raytracing_hit_inc.glsl must be available.
vec2 rt_uv_ddx;
vec2 rt_uv_ddy;
vec3 rt_position_ddx;
vec3 rt_position_ddy;

// Camera-pixel footprint projected onto the actual hit triangle. Works in
// closest-hit and any-hit, where raster derivative instructions are unavailable.
vec3 rt_pixel_plane_position(vec2 ndc, vec3 point, vec3 plane_normal) {
	vec4 a = scene_data_block.data.inv_projection_matrix * vec4(ndc, 0.5, 1.0);
	vec4 b = scene_data_block.data.inv_projection_matrix * vec4(ndc, 1.0, 1.0);
	a /= a.w;
	b /= b.w;
	mat4 camera = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0, 0, 0, 1)));
	vec3 origin = (camera * a).xyz;
	vec3 direction = mat3(camera) * (b.xyz - a.xyz);
	return origin + direction * (dot(point - origin, plane_normal) / dot(direction, plane_normal));
}

void rt_compute_material_footprint(uint geometry_index, vec3 hit_position) {
	rt_uv_ddx = vec2(0.0);
	rt_uv_ddy = vec2(0.0);
	rt_position_ddx = vec3(0.0);
	rt_position_ddy = vec3(0.0);
	GeometryData geom = geometries[geometry_index];
	// Procedural surfaces have no triangle vertices. Their shader can provide
	// STREAMING_LOD; without one the zero footprint requests full resolution.
	if ((geom.flags & FLAG_PROCEDURAL) != 0u || geom.vertex_address == 0ul) {
		return;
	}
	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	vec3 p0 = (gl_ObjectToWorldEXT * vec4(rt_triangle_position(geom, i0), 1.0)).xyz;
	vec3 e1 = (gl_ObjectToWorldEXT * vec4(rt_triangle_position(geom, i1), 1.0)).xyz - p0;
	vec3 e2 = (gl_ObjectToWorldEXT * vec4(rt_triangle_position(geom, i2), 1.0)).xyz - p0;
	vec3 plane_normal = cross(e1, e2);
	mat4 camera_view = transpose(mat4(scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0, 0, 0, 1)));
	vec4 clip = scene_data_block.data.projection_matrix * camera_view * vec4(hit_position, 1.0);
	vec2 ndc = clip.xy / clip.w;
	vec2 pixel = 2.0 / scene_data_block.data.viewport_size;
	rt_position_ddx = rt_pixel_plane_position(ndc + vec2(pixel.x, 0), hit_position, plane_normal) - hit_position;
	rt_position_ddy = rt_pixel_plane_position(ndc + vec2(0, pixel.y), hit_position, plane_normal) - hit_position;
	float a = dot(e1, e1), b = dot(e1, e2), c = dot(e2, e2);
	float determinant = a * c - b * b;
	if (determinant <= 0.0) {
		return;
	}
	mat2 inverse_metric = mat2(c, -b, -b, a) / determinant;
	vec2 bx = inverse_metric * vec2(dot(rt_position_ddx, e1), dot(rt_position_ddx, e2));
	vec2 by = inverse_metric * vec2(dot(rt_position_ddy, e1), dot(rt_position_ddy, e2));
	vec2 uv0 = fetch_uv(geom, i0, i1, i2, vec3(1, 0, 0));
	vec2 uv1 = fetch_uv(geom, i0, i1, i2, vec3(0, 1, 0)) - uv0;
	vec2 uv2 = fetch_uv(geom, i0, i1, i2, vec3(0, 0, 1)) - uv0;
	rt_uv_ddx = uv1 * bx.x + uv2 * bx.y;
	rt_uv_ddy = uv1 * by.x + uv2 * by.y;
}
