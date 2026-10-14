#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 16, local_size_y = 16 ) in;
//=============================================================================================================================
#include "common.h"
//=============================================================================================================================
layout ( rgba32f, set = 0, binding = 1 ) uniform image2D image; // accumulator image
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 2, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;
//=============================================================================================================================
void main () {
	vec4 color = vec4( 0.0f, 0.0f, 0.0f, 1.0f );

	vec2 pixelUV = ( gl_GlobalInvocationID.xy + 0.5f ) / GlobalData.presentBufferResolution - vec2( 0.5f );
	pixelUV.x /= ( GlobalData.presentBufferResolution.x / GlobalData.presentBufferResolution.y );
	pixelUV.y = -pixelUV.y;
	if ( distance( lens.params.mouseloc.xy, pixelUV ) < 0.01f ) {
		color = vec4( 1.0f, 0.0f, 0.0f, 0.0f );
	}
	imageStore( image, ivec2( gl_GlobalInvocationID.xy ), color );
}