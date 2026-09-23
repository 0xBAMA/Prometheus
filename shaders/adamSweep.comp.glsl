#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 16, local_size_y = 16 ) in;
//=============================================================================================================================
#include "common.h"
//=============================================================================================================================
layout ( rgba32f, set = 0, binding = 1 ) uniform image2D mipN;
layout ( rgba32f, set = 0, binding = 2 ) uniform image2D mipNPlusOne;
//=============================================================================================================================
void main () {
	ivec2 loc = ivec2( gl_GlobalInvocationID.xy );
	ivec2 halfLoc = loc * 2;
	if ( halfLoc.x < imageSize( mipN ).x && halfLoc.y < imageSize( mipN ).y ) {
		float count = 0.0f;
		vec3 sum = vec3( 0.0f );
		const ivec2 offsets[ 4 ] = ivec2[]( ivec2( 0, 0 ), ivec2( 1, 0 ), ivec2( 0, 1 ), ivec2( 1, 1 ) );
		for ( int i = 0; i < 4; i++ ) {
			// read the value stored in mip N
			vec4 valueSample = imageLoad( mipN, halfLoc + offsets[ i ] );
			// gather count, to be used as a normalization term on the weighted sum
			count += valueSample.a;
			sum += valueSample.rgb * valueSample.a;
		}

		// if we have any data for this pixel...
		vec4 result = vec4( 0.0f );
		if ( count != 0.0f ) {
			result = vec4( sum / count, count );
		}
		imageStore( mipNPlusOne, loc, result );
	}
}
