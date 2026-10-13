#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 16, local_size_y = 16 ) in;
//=============================================================================================================================
#include "common.h"
#include "random.h"
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 1, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;
//=============================================================================================================================
layout ( set = 0, binding = 2, scalar ) buffer lensSegmentScratch {
	float wavelengths[ 2048 ]; // to match invocation count
	vec2 pathSegmentPoints[ 2048 * 64 ];
} scratch;
//=============================================================================================================================
void main () {
	seed = gl_GlobalInvocationID.x * 8675309 + PushConstants.wangSeed;

	int index = int( gl_GlobalInvocationID.x );
	scratch.wavelengths[ index ] = remap( rFloat(), 0.0f, 1.0f, 380.0f, 830.0f );

	// used for indexing memory
	int pathBaseIdx = index * 64;

	// attempting to shoot a stochastically sampled ray through the lens

	// for now placeholder segments
	float maxDim = max( 2.0f * lens.params.maxSemiAperture, lens.params.totalSystemThickness );
	for ( int i = 0; i < 64; i++ ) {
		scratch.pathSegmentPoints[ pathBaseIdx + i ] = vec2(
			remap( rFloat(), 0.0f, 1.0f, 0.1f * maxDim, -1.1 * maxDim ),
			remap( rFloat(), 0.0f, 1.0f, 0.6f * maxDim, -0.6f * maxDim )
		);
	}

}

