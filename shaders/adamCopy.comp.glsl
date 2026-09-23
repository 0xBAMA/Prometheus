#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 16, local_size_y = 16 ) in;
//=============================================================================================================================
#include "common.h"
//=============================================================================================================================
layout ( r32ui, set = 0, binding = 1 ) uniform uimage2D RTally;
layout ( r32ui, set = 0, binding = 2 ) uniform uimage2D GTally;
layout ( r32ui, set = 0, binding = 3 ) uniform uimage2D BTally;
layout ( r32ui, set = 0, binding = 4 ) uniform uimage2D CTally;
layout ( rgba32f, set = 0, binding = 5 ) uniform image2D AdamMip0;
//=============================================================================================================================
void main () {
	// bounds check
	ivec2 loc = ivec2( gl_GlobalInvocationID.xy );
	if ( GlobalData.reset == 0 ) {
		if ( loc.x < GlobalData.presentBufferResolution.x && loc.y < GlobalData.presentBufferResolution.y ) {
			// unapply fixed point scaling from tally operation
			const float r = float( imageLoad( RTally, loc ).r ) / 1024.0f;
			const float g = float( imageLoad( GTally, loc ).r ) / 1024.0f;
			const float b = float( imageLoad( BTally, loc ).r ) / 1024.0f;
			const float c = float( imageLoad( CTally, loc ).r );

			if (c != 0.0f) {
				// storing normalized data to mip 0 of the Adam texture
				imageStore( AdamMip0, loc, vec4( r / c, g / c, b / c, c ) );
			}
		} else {
			// can expand copy to full image, if an image clear is needed
			imageStore( AdamMip0, loc, vec4( 0.0f ) ); // this is cheap
		}
	} else {
		// wiping tallies
		imageStore( RTally, loc, uvec4( 0 ) );
		imageStore( GTally, loc, uvec4( 0 ) );
		imageStore( BTally, loc, uvec4( 0 ) );
		imageStore( CTally, loc, uvec4( 0 ) );
		imageStore( AdamMip0, loc, vec4( 0.0f ) );
	}
}
