#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 256, local_size_y = 1 ) in;
//=============================================================================================================================
#include "common.h"
#include "random.h"
#include "rayState.h"
//=============================================================================================================================
layout ( set = 0, binding = 1 ) buffer rayBuffer {
	rayState_t rays[];
};
//=============================================================================================================================
// todo: film sensitivity LUTs, traced like the light LUTs -> importance sampling basis for wavelength
//=============================================================================================================================
void main () {

	uint idx = gl_GlobalInvocationID.x;
	seed = PushConstants.wangSeed + 8675309 * idx;

	// this shader is responsible for generating the initial camera rays
		// tbd how the pixel offsets are sourced, not sure yet

	rayState_t ray;
	StateReset( ray );

	// key initial values:
		// origin
		// direction
		// wavelength
		// pixel index

}
