#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 256, local_size_y = 1 ) in;
//=============================================================================================================================
#include "common.h"
#include "random.h"
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 1, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;
//=============================================================================================================================
layout( set = 0, binding = 2, scalar ) buffer pupilBounds {
	vec4 bounds[ 4096 ];
} pupilLUT;
//=============================================================================================================================
void main () {

	int idx = int( gl_GlobalInvocationID.x );
	seed = idx * 8675309 + PushConstants.wangSeed;

	// so this invocation is handling a ray which starts at some position along the Y axis, based on the current value of idx, the invocation ID
	vec3 sourcePoint = vec3( 0.0f, remap( idx, 0.0f, 4095.0f, 0.0f, length( lens.params.filmSize.xy / 2.0f ) ), -lens.params.totalSystemThickness + lens.params.focusAdjust );

	// initializing the bounds tracking
	float minSampleX = 1.0f, maxSampleX = -1.0f;
	float minSampleY = 1.0f, maxSampleY = -1.0f;

	for ( int i = 0; i < 64000; i++ ) {
		 vec2 rearElementSpaceSamplePos = randCircle( getWeylOffset( i ) );
		float wavelength = remap( rFloat(), 0.0f, 1.0f, 400.0f, 700.0f );

		// create a ray from this invocation's sampling point, to the specified point
		vec3 rayDirection = normalize( vec3( rearElementSpaceSamplePos * lens.params.rearElementSA, -lens.params.rearElementZ ) - sourcePoint );
		vec3 rayOrigin = sourcePoint;

		// trace the specified ray through the system
		int j = 0;
		for ( j = 0; j < lens.params.numElements; j++ ) {

			float radius = lens.params.elements[ j ].radius;
			float axisPosition = lens.params.elements[ j ].axisPos - radius;
			float cosTerm = lens.params.elements[ j ].cosTerm;
			vec2 materialBack = vec2( lens.params.elements[ j ].backCauchyA, lens.params.elements[ j ].backCauchyB );
			vec2 materialFront = vec2( lens.params.elements[ j ].frontCauchyA, lens.params.elements[ j ].frontCauchyB );
			int interfaceCode = lens.params.elements[ j ].interfaceCode;

			vec3 normal = vec3( 0.0f );
			float t = 0.0f;
			if ( radius == 0.0f ) {
				// using planar intersection
				const vec3 planeNormal = vec3( 0.0f, 0.0f, -1.0f );
				t = rayPlaneIntersect( planeNormal, vec3( 0.0f, 0.0f, axisPosition ), rayOrigin, rayDirection );

				float semiaperture = interfaceCode == 0 ? cosTerm / lens.params.apertureAdjust : cosTerm;
				if ( length( ( rayOrigin + rayDirection * t ).xy ) < semiaperture ) { // reused memory for semiaperture
					normal = planeNormal;
				} else {
					t = 1e30f; // invalid hit, outside of aperture
				}
			} else {
				// spherical element is specified
				t = sphereCapIntersect( vec3( 0.0f, 0.0f, axisPosition ), radius, vec3( 0.0f, 0.0f, ( radius < 0.0f ) ? -1.0f : 1.0f ), cosTerm, rayOrigin, rayDirection, normal );
			}

			if ( !( t < 1e9f && t > 0.0f ) ) {
				// this ray has not found a valid intersection
				break;
			} else {
				// continuing, prep for next iteration
				rayOrigin = rayOrigin + t * rayDirection;
				rayDirection = refract( rayDirection, normal, evaluateCauchy( materialBack.x, materialBack.y, wavelength ) / evaluateCauchy( materialFront.x, materialFront.y, wavelength ) );
			}
		}

		// if this sample goes all the way through and gets a valid hit on the front element, update min, max, based on the rearElementSpaceSamplePos
		if ( j == lens.params.numElements ) {
			minSampleX = min( minSampleX, rearElementSpaceSamplePos.x );
			maxSampleX = max( maxSampleX, rearElementSpaceSamplePos.x );
			minSampleY = min( minSampleY, rearElementSpaceSamplePos.y );
			maxSampleY = max( maxSampleY, rearElementSpaceSamplePos.y );
		}

	}

	// if min > max, we got no valid samples, but can detect that on the read end
	pupilLUT.bounds[ idx ].x = minSampleX;
	pupilLUT.bounds[ idx ].y = maxSampleX;
	pupilLUT.bounds[ idx ].z = minSampleY;
	pupilLUT.bounds[ idx ].w = maxSampleY;
}