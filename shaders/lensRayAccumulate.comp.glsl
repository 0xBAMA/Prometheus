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
layout ( set = 0, binding = 1 ) uniform sampler2D lensRayRasterColorAttachment;
layout ( rgba32f, set = 0, binding = 2 ) uniform image2D accumulator;
//=============================================================================================================================
void main () {
	const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );

	// load prior value from the accumulator
	vec4 accumulatorValue = imageLoad( accumulator, pixel );

	// load the value from the raster target
	vec4 rasterValue = texture( lensRayRasterColorAttachment, vec2( pixel + 0.5f ) / 512.0f );

	// compute the sum
	vec4 storeBack;
	storeBack = accumulatorValue;
	storeBack.rgb += rasterValue.rgb;
	storeBack.a += 1; // just doing this as a simple sample count right now

	// store the sum back to the accumulator -> normalization factor stored in .a? tbd
	imageStore( accumulator, pixel, storeBack );

}
