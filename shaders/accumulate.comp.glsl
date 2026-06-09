#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

layout ( local_size_x = 16, local_size_y = 16 ) in;

#include "common.h"
#include "random.h"

// raster color attachment and the accumulator
layout ( set = 0, binding = 1 ) uniform sampler2D rasterImage;
layout ( rgba32f, set = 0, binding = 2 ) uniform image2D accumulator;

void main () {
	seed = PushConstants.wangSeed + 42069 * gl_GlobalInvocationID.x + 8675309 * gl_GlobalInvocationID.y;

	// get the sum
	const ivec2 loc = ivec2( gl_GlobalInvocationID.xy );

	vec4 rasterContribution = texture( rasterImage, ( vec2( loc ) + vec2( NormalizedRandomFloat(), NormalizedRandomFloat() ) ) / ( textureSize( rasterImage, 0 ) / GlobalData.resolutionScalar ) );
	vec4 accumulatorContents = imageLoad( accumulator, loc );

	vec4 sum = rasterContribution + accumulatorContents;

	if ( GlobalData.reset != 0 )
		sum = rasterContribution;

	// store the result
	 imageStore( accumulator, loc, sum );
}