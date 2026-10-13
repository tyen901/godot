// Native projector data; layout matches RT_DecalData. The table is ordered by
// instance sorting offset, with RID as the stable tie breaker.
struct RTDecalData {
	mat4 model;
	mat4 inverse_model;
	vec4 instance_custom;
	vec4 modulate;
	vec4 fades;
	vec4 parameters;
	uvec4 textures;
	uint64_t uniform_address;
	uint callable_index;
	uint cull_mask;
	uint material_feedback_index;
	uint material_feedback_pad0;
	uint material_feedback_pad1;
	uint material_feedback_pad2;
};
layout(set = 0, binding = 33) uniform accelerationStructureEXT decal_tlas;
layout(set = 0, binding = 34, std430) readonly buffer DecalBuffer {
	uint decal_count;
	uint decal_pad0; uint decal_pad1; uint decal_pad2;
	RTDecalData rt_decals[];
};
struct RTDecalCall {
	uint index;
	vec3 position;
	vec3 original_normal;
	vec3 dx;
	vec3 dy;
	vec3 albedo_add;
	vec3 albedo_retain;
	vec3 normal_smoothness;
	float material_coverage;
	float metalness;
	float transmission;
	vec3 emission;
	float specular;
};
