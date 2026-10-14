#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
#include "common.h"
#include "random.h"
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 1, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;

layout( set = 0, binding = 2, scalar ) readonly buffer lensOutlines {
	vec3 points[];
};

layout ( location = 0 ) out vec3 color;

void main () {
	// initializing the RNG
	seed = PushConstants.wangSeed + 8675309 * gl_InstanceIndex; // seeding per instance, allows me to jitter the lines consistently

	// raytracing against the list of lens elements, starting at the zero plane
	// the extents of the image are determined by the maximum of the horizontal and vertical extents
	float maxDim = max( 2.0f * lens.params.maxSemiAperture, lens.params.totalSystemThickness );

	vec2 jitter = rFloatN2() / 512.0f;

	gl_Position = vec4(
		remap( -points[ gl_VertexIndex ].x, 0.1f * maxDim, -1.1 * maxDim, -1.0f, 1.0f ) + jitter.x,
		remap( points[ gl_VertexIndex ].y, 0.6f * maxDim, -0.6f * maxDim, -1.0f, 1.0f ) + jitter.y,
		0.5f, 1.0f );

	if ( points[ gl_VertexIndex ].z == -1.0f ) {

		// this is a segment associated with the edges of the lenses
		color = vec3( 0.618f );

	} else if ( points[ gl_VertexIndex ].z == -2.0f ) {

		// this is a line associated with the film plane
		color = vec3( 1.0f );

		// I'm going to hijack this case to resize the drawn film plane without rewriting the memory for the segments
		float pos = 0.0f;
		if ( points[ gl_VertexIndex ].y > 0.0f ) {
			pos = lens.params.filmSize.y / 2.0f;
		} else {
			pos = -lens.params.filmSize.y / 2.0f;
		}
		gl_Position.y = remap( pos, 0.6f * maxDim, -0.6f * maxDim, -1.0f, 1.0f ) + jitter.y;

	} else {
		color = vec3( sin( points[ gl_VertexIndex ].z - 0.95f ) / 2.0f + 0.5f, cos( points[ gl_VertexIndex ].z + 0.9f ) / 2.0f + 0.5f, sin( points[ gl_VertexIndex ].z + 0.5f ) / 2.0f + 0.5f );
	}
}