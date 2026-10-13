// Custom shader globals for RT hit groups (closest-hit and any-hit).
// Include under #ifdef RT_CUSTOM_HIT_GROUP, outside main().
// Requires: bindless_textures[], materials[] bindings already declared.
#ifdef RT_STAGE_ANY_HIT
#define rt_discard() ignoreIntersectionEXT
#else
// Source discard was already evaluated before closest-hit is invoked.
#define rt_discard()
#endif

layout(buffer_reference, std140) readonly buffer CustomMaterialUniforms{
	/* RT_CUSTOM_UNIFORM_MEMBERS */
};

// Shared vertex/fragment built-ins at global scope so that the vertex function,
// texture #defines, and fragment_globals functions can all access them.
// Assigned to real hit values in main() before use.
CustomMaterialUniforms material = CustomMaterialUniforms(uint64_t(0));
vec3 vertex = vec3(0.0);
vec3 normal = vec3(0.0, 0.0, 1.0);
vec3 tangent = vec3(1.0, 0.0, 0.0);
vec3 binormal = vec3(0.0, 1.0, 0.0);
vec2 uv_interp = vec2(0.0);
vec2 uv2_interp = vec2(0.0);
vec3 rt_object_ddx;
vec3 rt_object_ddy;
uint rt_instance_uniforms_ofs = 0u;
float roughness = 0.5; // Vertex ROUGHNESS is shared with the fragment stage.
// Declared before injected helper functions, including when streaming is disabled.
float streaming_lod = 32.0;
vec4 instance_custom;
vec4 custom0_attrib;
vec4 custom1_attrib;
vec4 custom2_attrib;
vec4 custom3_attrib;

vec4 rt_custom_vertex_attribute(GeometryData geom, uint index, uint channel) {
	uint descriptor = geom.custom_attribute[channel];
	if (descriptor == OFFSET_NONE) {
		return vec4(0.0);
	}
	uint offset = (index * geom.attribute_stride + (descriptor & 0x0fffffffu)) >> 2u;
	uint format = descriptor >> 28u;
	Uint32Buffer data = Uint32Buffer(geom.attribute_address);
	uint first = data.v[offset];
	if (format == 0u) {
		return unpackUnorm4x8(first);
	}
	if (format == 1u) {
		return unpackSnorm4x8(first);
	}
	if (format == 2u) {
		return vec4(unpackHalf2x16(first), 0.0, 1.0);
	}
	if (format == 3u) {
		return vec4(unpackHalf2x16(first), unpackHalf2x16(data.v[offset + 1u]));
	}
	vec4 result = vec4(uintBitsToFloat(first), 0.0, 0.0, 1.0);
	if (format >= 5u) {
		result.y = uintBitsToFloat(data.v[offset + 1u]);
	}
	if (format >= 6u) {
		result.z = uintBitsToFloat(data.v[offset + 2u]);
	}
	if (format == 7u) {
		result.w = uintBitsToFloat(data.v[offset + 3u]);
	}
	return result;
}

vec4 rt_custom_attribute(GeometryData geom, uint channel) {
	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
	return rt_custom_vertex_attribute(geom, i0, channel) * bary.x + rt_custom_vertex_attribute(geom, i1, channel) * bary.y + rt_custom_vertex_attribute(geom, i2, channel) * bary.z;
}

vec4 color_interp = vec4(1.0);
vec3 view = vec3(0.0, 0.0, -1.0);
mat4 read_model_matrix = mat4(1.0);
mat3 model_normal_matrix = mat3(1.0);
mat4 read_view_matrix = mat4(1.0);
mat4 inv_view_matrix = mat4(1.0);
mat4 projection_matrix = mat4(1.0);
mat4 inv_projection_matrix = mat4(1.0);
float global_time = 0.0;
vec2 read_viewport_size = vec2(1.0);
bool rt_front_facing = true;
vec2 rt_screen_uv = vec2(0.0);
vec4 rt_frag_coord = vec4(0.0);
float alpha_antialiasing_edge = 0.0;
vec2 alpha_texture_coordinate = vec2(0.0);

// Screen/depth textures are unavailable in RT -- alias to bindless slot 0
// so shaders that reference them still compile (reads return dummy values).
#define depth_buffer bindless_textures[0]
#define color_buffer bindless_textures[0]
#define normal_roughness_buffer bindless_textures[0]

/* RT_CUSTOM_TEXTURE_DEFINES */
/* RT_CUSTOM_FRAGMENT_GLOBALS */

/* RT_CUSTOM_VERTEX_FUNCTION */
