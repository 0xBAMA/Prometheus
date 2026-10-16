#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 256 ) in;
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
	vec2 pathSegmentPoints[ 2048 * 128 ];
} scratch;
//=============================================================================================================================
void wipeBuffer () {
	int index = int( gl_GlobalInvocationID.x );
	// initially fill this portion of the buffer with invalid segment data
	for ( int i = 0; i < 64; i++ ) { // any data not touched later in the shader will retain this, and not generate valid path segments
		scratch.pathSegmentPoints[ 128 * index + 2 * i ] = vec2( 1e9f );
		scratch.pathSegmentPoints[ 128 * index + 2 * i + 1 ] = vec2( 1e9f );
	}
}
//=============================================================================================================================
void main () {
	seed = gl_GlobalInvocationID.x * 8675309 + PushConstants.wangSeed;

	int index = int( gl_GlobalInvocationID.x );
	float wavelength = scratch.wavelengths[ index ] = remap( rFloat(), 0.0f, 1.0f, 380.0f, 830.0f );

	wipeBuffer();

	// attempting to shoot a stochastically sampled ray through the lens - our ray starts between two points:
		// a specified point on the film plane
		// a randomly selected point on the rear element

	vec3 filmPlanePoint = vec3( ( lens.params.mouseloc ) * lens.params.filmSize, -lens.params.totalSystemThickness + lens.params.focusAdjust );
	vec3 rearElementPoint = vec3(  CircleOffset() * lens.params.rearElementSA, -lens.params.rearElementZ );

	// now we are trying to trace a path through the lens

	// matching to the lens system orientation
	vec3 rayOrigin = filmPlanePoint;
	vec3 rayDirection = normalize( rearElementPoint - filmPlanePoint );

	int i = 0;
	for ( i = 0; i < lens.params.numElements; i++ ) {

		float radius = lens.params.elements[ i ].radius;
		float axisPosition = lens.params.elements[ i ].axisPos - radius;
		float cosTerm = lens.params.elements[ i ].cosTerm;
		vec2 materialBack = vec2( lens.params.elements[ i ].backCauchyA, lens.params.elements[ i ].backCauchyB );
		vec2 materialFront = vec2( lens.params.elements[ i ].frontCauchyA, lens.params.elements[ i ].frontCauchyB );

		vec3 normal = vec3( 0.0f );
		float t = 0.0f;
		if ( radius == 0.0f ) {
			// using planar intersection
			 const vec3 planeNormal = vec3( 0.0f, 0.0f, -1.0f );
			 t = rayPlaneIntersect( planeNormal, vec3( 0.0f, 0.0f, axisPosition ), rayOrigin, rayDirection );
			 if ( length( ( rayOrigin + rayDirection * t ).xy ) < cosTerm ) { // reused memory for semiaperture
				 normal = planeNormal;
			 } else {
				 t = 1e30f; // invalid hit, outside of aperture
			 }
		} else {
			// spherical element is specified
			t = sphereCapIntersect( vec3( 0.0f, 0.0f, axisPosition ), radius, vec3( 0.0f, 0.0f, ( radius < 0.0f ) ? -1.0f : 1.0f ), cosTerm, rayOrigin, rayDirection, normal );
		}

		if ( !( t < 1e9f && t > 0.0f ) ) {
			// no intersection found, don't draw anything
//			wipeBuffer(); // ends up too dim
			return;
		} else {
			// draw a segment
			scratch.pathSegmentPoints[ 128 * index + 2 * i ] = rayOrigin.zy;
			scratch.pathSegmentPoints[ 128 * index + 2 * i + 1 ] = ( rayOrigin + t * rayDirection ).zy;

			// prep for next iteration
			rayOrigin = rayOrigin + t * rayDirection;
			rayDirection = refract( rayDirection, normal, evaluateCauchy( materialBack.x, materialBack.y, wavelength ) / evaluateCauchy( materialFront.x, materialFront.y, wavelength ) );
		}
	}

	// if the ray has reached the final element ("escaped" rays are valid for rendering), draw the ray along its continued path
	scratch.pathSegmentPoints[ 128 * index + 2 * i ] = rayOrigin.zy;
	scratch.pathSegmentPoints[ 128 * index + 2 * i + 1 ] = ( rayOrigin + 10000.0f * rayDirection ).zy;
}

