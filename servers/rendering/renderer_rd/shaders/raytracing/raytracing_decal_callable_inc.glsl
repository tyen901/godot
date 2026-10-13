layout(set = 1, binding = 0) uniform texture2D bindless_textures[];
layout(location = 0) callableDataInEXT RTDecalCall decal_call;
layout(buffer_reference, std140) readonly buffer CustomMaterialUniforms{
	/* RT_CUSTOM_UNIFORM_MEMBERS */
};
CustomMaterialUniforms material;
vec3 vertex, normal, tangent, binormal;
vec2 uv_interp, uv2_interp, rt_uv_ddx, rt_uv_ddy;
// Declared before injected helper functions, including when streaming is disabled.
float streaming_lod = 32.0;
vec3 rt_position_ddx, rt_position_ddy, rt_object_ddx, rt_object_ddy;
mat4 read_model_matrix, read_view_matrix, inv_view_matrix, projection_matrix, inv_projection_matrix;
mat3 model_normal_matrix;
vec4 instance_custom, color_interp;
vec2 read_viewport_size;
float global_time, roughness;
uint rt_instance_uniforms_ofs;
// A discarded projector leaves the receiver unchanged.
#define rt_discard() return
/* RT_CUSTOM_TEXTURE_DEFINES */
/* RT_CUSTOM_FRAGMENT_GLOBALS */
void main() {
	RTDecalData d = rt_decals[decal_call.index];
	material = CustomMaterialUniforms(d.uniform_address);
	read_model_matrix = d.model;
	model_normal_matrix = transpose(mat3(d.inverse_model));
	read_view_matrix = mat4(1.0);
	inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0, 0, 0, 1)));
	projection_matrix = scene_data_block.data.projection_matrix;
	inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
	vertex = decal_call.position;
	normal = decal_call.original_normal;
	tangent = normalize(d.model[0].xyz);
	binormal = normalize(d.model[2].xyz);
	instance_custom = d.instance_custom;
	color_interp = d.modulate;
	vec3 local = (d.inverse_model * vec4(vertex, 1.0)).xyz;
	uv_interp = local.xz + 0.5;
	rt_object_ddx = mat3(d.inverse_model) * decal_call.dx;
	rt_object_ddy = mat3(d.inverse_model) * decal_call.dy;
	rt_uv_ddx = rt_object_ddx.xz;
	rt_uv_ddy = rt_object_ddy.xz;
	rt_position_ddx = decal_call.dx;
	rt_position_ddy = decal_call.dy;
	global_time = scene_data_block.data.time;
	read_viewport_size = scene_data_block.data.viewport_size;
	vec3 albedo = vec3(1.0), emission = decal_call.emission, backlight = vec3(0.0);
	float alpha = 1.0, metallic = 0.0, specular = 0.5, premul_alpha = 1.0;
	float transmission = decal_call.transmission;
	float ao = 1.0, ao_light_affect = 0.0, normal_map_depth = 1.0;
	vec3 normal_map = vec3(0.5, 0.5, 1.0);
	roughness = 1.0;
	/* RT_CUSTOM_FRAGMENT_CODE */
#ifdef TEXTURE_STREAMING
#ifdef STREAMING_LOD_USED
	rt_write_material_feedback(d.material_feedback_index, streaming_footprint_sq_from_lod(streaming_lod));
#else
	rt_write_material_feedback(d.material_feedback_index, streaming_footprint_sq(rt_uv_ddx, rt_uv_ddy));
#endif
#endif
	alpha = clamp(alpha, 0.0, 1.0);
	decal_call.albedo_add = albedo * alpha;
	decal_call.albedo_retain = vec3(clamp(1.0 - alpha * premul_alpha, 0.0, 1.0));
#ifdef RT_DECAL_BLEND_MUL
	decal_call.albedo_add = vec3(0.0);
	decal_call.albedo_retain = mix(vec3(1.0), albedo, alpha);
	decal_call.material_coverage = 0.0;
#else
	decal_call.material_coverage = alpha;
#endif
	decal_call.normal_smoothness = normalize(normal) * (0.375 + 0.125 * (1.0 - clamp(roughness, 0.0, 1.0)));
	decal_call.metalness = metallic;
	decal_call.transmission = transmission;
	decal_call.emission = emission;
	decal_call.specular = specular;
}
