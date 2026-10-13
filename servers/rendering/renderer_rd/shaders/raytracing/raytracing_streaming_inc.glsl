// Shares the native streaming buffer and encoding with raster feedback.
#ifdef TEXTURE_STREAMING
#include "../scene_forward_streaming_inc.glsl"

layout(set = 0, binding = 35, std430) buffer RTMaterialFeedbackBuffer {
	uint data[];
}
rt_material_feedback;

void rt_write_material_feedback(uint material_index, float footprint_sq) {
	// The native buffer may not have grown to a newly allocated material yet.
	// UINT32_MAX also prevents writes to the dummy binding while streaming stops.
	if (material_index == 0xffffffffu || material_index >= uint(rt_material_feedback.data.length())) {
		return;
	}
	if (isnan(footprint_sq) || isinf(footprint_sq) || footprint_sq < 0.0) {
		return;
	}
	// Matches TextureStreaming::_process_material_feedback_buffer. Adjacent rays
	// need not hit the same material, so no subgroup reduction is valid here.
	atomicMax(rt_material_feedback.data[material_index], ~floatBitsToUint(footprint_sq));
}
#endif
