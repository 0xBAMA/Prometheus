#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 16, local_size_y = 16 ) in;
//=============================================================================================================================
#include "common.h"
//=============================================================================================================================
layout ( set = 0, binding = 1 ) uniform sampler2D Adam;
layout ( rgba32f, set = 0, binding = 2 ) uniform image2D accumulator;
//=============================================================================================================================
void main () {
	// so we want to iterate through the mip chain, till we find the first mip that has data
	// proceeding from coarsest to finest level of refinement, till we find the first nonzero count
	vec3 color = vec3( 0.0f );
	vec4 value = vec4( 0.0f );

	ivec2 loc = ivec2( gl_GlobalInvocationID.xy );
	const int numLevels = textureQueryLevels( Adam );
	for ( int i = 0; i <= numLevels; i++ ) {
		value = texelFetch( Adam, loc, i );
		if ( value.a != 0.0f ) {
			color = value.rgb;
			// color = vec3( 1.0f / max( float( i ), 1.0f ) ); // debug color
			break;
		}
		loc /= 2;
	}

	// result into accumulator
	imageStore( accumulator, ivec2( gl_GlobalInvocationID.xy ), vec4( color, 1.0f ) );
}