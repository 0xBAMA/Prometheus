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
layout ( rgba16f, set = 0, binding = 1 ) uniform image2D image;
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 2, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;
//=============================================================================================================================
void main () {
	// initializing the RNG
	ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
	seed = PushConstants.wangSeed + 8675309 * pixel.x + 42069 * pixel.y;

	// raytracing against the list of lens elements, starting at the zero plane
		// the extents of the image are determined by the maximum of the horizontal and vertical extents
	float maxDim = max( 2.0f * lens.params.maxSemiAperture, lens.params.totalSystemThickness );

	vec4 colorContribution = vec4( 0.0f );
	for ( int i = 0; i < 16; i++ ) {
		vec2 jitter = rFloatN2();

		// the image that we are drawing to is 512px square, so the scaling on each axis should be uniform
		vec2 myPos = vec2(// overall scaling is based on 1.2x the max dimension, to give some margin
		remap(pixel.x + jitter.x, 0.0f, 511.0f, 0.1f * maxDim, -1.1 * maxDim),
		remap(pixel.y + jitter.y, 0.0f, 511.0f, 0.6f * maxDim, -0.6f * maxDim)
		);

		// matching to the lens system orientation
		vec3 rayOrigin = vec3(0.0f, myPos.y, myPos.x);
		vec3 rayDirection = vec3(1.0f, 0.0f, 0.0f);

		float dClosest = 1e9f;
		int iClosest = -1;
		for (int i = 0; i < lens.params.numElements; i++) {
			float radius = lens.params.elements[i].radius;
			float axisPosition = lens.params.elements[i].axisPos - radius;
			float cosTerm = lens.params.elements[i].cosTerm;
			vec2 materialBack = unpackHalf2x16(floatBitsToUint(lens.params.elements[i].materialBack));
			vec2 materialFront = unpackHalf2x16(floatBitsToUint(lens.params.elements[i].materialFront));

			// plano elements are orthogonal to this preview, so it doesn't really make sense to draw them
			float t = 0.0f;
			//		const vec3 planeNormal = vec3( 0.0f, 0.0f, -1.0f );
			if (radius == 0.0f) {
				// using planar intersection
				//			t = rayPlaneIntersect( planeNormal, vec3( 0.0f, 0.0f, axisPosition ), ray.origin, ray.direction );
				//			if ( length( ( ray.origin + ray.direction * t ).xy ) < cosTerm ) { // reused memory for semiaperture
				//				normal = planeNormal;
				//			}
			} else {
				// spherical element is specified
				vec3 normal = vec3(0.f);
				t = sphereCapIntersect(vec3(0.0f, 0.0f, axisPosition), radius, vec3(0.0f, 0.0f, (radius < 0.0f) ? -1.0f : 1.0f), cosTerm, rayOrigin, rayDirection, normal);
			}

			// update the closest intersected element
			if (t < dClosest && t > 0.0f) {
				dClosest = t;
				iClosest = i;
			}
		}

		if ((myPos.x < 0.0f && myPos.x > -maxDim) && abs(myPos.y) < lens.params.maxSemiAperture) {
			colorContribution += vec4( vec3( 0.1f ), 1.0f );
			if (iClosest != -1) {
				colorContribution += vec4( vec3( sin( iClosest + 0.5f ) / 2.0f + 0.5f, cos( iClosest ) / 2.0f + 0.5f, sin( iClosest ) / 2.0f + 0.5f ), 1.0f );
			}
		} else {
			colorContribution += vec4( vec3( checkerBoard( 0.1f, vec3( pixel, 0.5f ) ) ), 1.0f );
		}
	}
	imageStore( image, pixel, colorContribution / 16.0f );
}