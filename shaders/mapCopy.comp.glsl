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
layout ( set = 0, binding = 1 ) uniform sampler2D rasterBuffer; // result of the map draws
layout ( rgba32f, set = 0, binding = 2 ) uniform image2D image; // accumulator image
//=============================================================================================================================
void main () {
//=============================================================================================================================
	// initializing the RNG
	const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
	seed = PushConstants.wangSeed + 8675309 * pixel.x + 42069 * pixel.y;
//=============================================================================================================================
	// pixel value will be sampled out of the buffer containing the raster results for the map
	vec2 uv = ( pixel + vec2( 0.5f ) ) / ( GlobalData.presentBufferResolution );
	int numSamples = 100;
	vec3 accum = vec3( 0.0f );
	for ( int i = 0; i < numSamples; i++ ) {
		vec2 offset = 0.003f * rnd_disc_cauchy();
		vec2 samplePosition = uv + offset;
		accum += texture( rasterBuffer, samplePosition ).xyz * ( 0.02f / max( length( offset ), 0.001f ) );
	}
//=============================================================================================================================
	imageStore( image, pixel, vec4( accum / numSamples, 1.0f ) );
}