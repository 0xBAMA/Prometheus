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
void main () {
	seed = gl_GlobalInvocationID.x * 8675309 + PushConstants.wangSeed;

	int index = int( gl_GlobalInvocationID.x );

	// attempting to shoot a stochastically sampled ray through the lens - our ray starts between two points:
		// a specified point on the film plane
		// a randomly selected point on the rear element

	vec3 filmPlanePoint = vec3( ( lens.params.mouseloc ) * lens.params.filmSize, -lens.params.totalSystemThickness );
	vec3 rearElementPoint = vec3(  CircleOffset() * lens.params.rearElementSA, -lens.params.rearElementZ );

	// now we are trying to trace a path through the lens

	// matching to the lens system orientation
	vec3 rayOrigin = filmPlanePoint;
	vec3 rayDirection = normalize( rearElementPoint - filmPlanePoint );

	vec3 nextSegmentPoint;
	vec3 nextSegmentDir;

	float dClosest = 1e9f;
	int iClosest = -1;
	vec3 nClosest = vec3( 0.0f );
	for ( int i = 0; i < lens.params.numElements; i++ ) {

		float radius = lens.params.elements[ i ].radius;
		float axisPosition = lens.params.elements[ i ].axisPos - radius;
		float cosTerm = lens.params.elements[ i ].cosTerm;
		vec2 materialBack = vec2( lens.params.elements[ i ].backCauchyA, lens.params.elements[ i ].backCauchyB );
		vec2 materialFront = vec2( lens.params.elements[ i ].frontCauchyA, lens.params.elements[ i ].frontCauchyB );

		vec3 normal = vec3( 0.0f );
		float t = 0.0f;
		if ( radius == 0.0f ) {
			// using planar intersection
			// const vec3 planeNormal = vec3( 0.0f, 0.0f, -1.0f );
			// t = rayPlaneIntersect( planeNormal, vec3( 0.0f, 0.0f, axisPosition ), ray.origin, ray.direction );
			// if ( length( ( ray.origin + ray.direction * t ).xy ) < cosTerm ) { // reused memory for semiaperture
				// normal = planeNormal;
			// }
		} else {
			// spherical element is specified
			t = sphereCapIntersect( vec3( 0.0f, 0.0f, axisPosition ), radius, vec3( 0.0f, 0.0f, ( radius < 0.0f ) ? -1.0f : 1.0f ), cosTerm, rayOrigin, rayDirection, normal );
			nextSegmentPoint = rayOrigin + rayDirection * t;
			nextSegmentDir = refract( rayDirection, normal, ( materialBack.x / materialFront.y ) );
		}

		// update the closest intersected element
		if ( t < dClosest && t > 0.0f ) {
			dClosest = t;
			iClosest = i;
			nClosest = normal;
		}
	}

	vec2 p0, p1, p2;
	if ( iClosest != -1 ) {
		p0 = ( rayOrigin ).zy;
		p1 = ( nextSegmentPoint ).zy;
		p2 = ( nextSegmentPoint + 100.0f * nextSegmentDir ).zy;
		scratch.wavelengths[ index ] = remap( rFloat(), 0.0f, 1.0f, 380.0f, 830.0f );
	} else {
		p0 = p1 = vec2( 1e30f );
		scratch.wavelengths[ index ] = 0.0f;
	}

	// used for indexing memory
	int pathBaseIdx = index * 64;
	for ( int i = 0; i < 64; i += 3 ) {
		scratch.pathSegmentPoints[ pathBaseIdx + i ] = p0;
		scratch.pathSegmentPoints[ pathBaseIdx + i + 1 ] = p1;
		scratch.pathSegmentPoints[ pathBaseIdx + i + 2 ] = p2;
	}

}

