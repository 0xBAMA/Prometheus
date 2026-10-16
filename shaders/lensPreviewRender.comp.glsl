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
layout ( rgba32f, set = 0, binding = 1 ) uniform image2D image; // accumulator image
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 2, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;
//=============================================================================================================================
layout( set = 0, binding = 3, scalar ) buffer pupilBounds {
	vec4 bounds[ 1024 ];
} pupilLUT;
//=============================================================================================================================
void main () {
	vec4 color = vec4( 0.0f, 0.0f, 0.0f, 1.0f );

	seed = 8675309 * gl_GlobalInvocationID.x + 42069 * gl_GlobalInvocationID.y + PushConstants.wangSeed;

	// determine my position on the film
	vec2 pixelUV = ( gl_GlobalInvocationID.xy + 0.5f ) / GlobalData.presentBufferResolution - vec2( 0.5f );
	pixelUV.x /= ( GlobalData.presentBufferResolution.x / GlobalData.presentBufferResolution.y );
	pixelUV.y = -pixelUV.y;

	// the film is then mapped to the screen

	vec2 uv = pixelUV * ( lens.params.filmSize.xy );

	int LUTentry = int( floor( remap( length( uv ) / length( lens.params.filmSize.xy / 2.0f ), 0.0f, 1.0f, 0.0f, 1024.f ) ) );

	float minSampleX = pupilLUT.bounds[ LUTentry ].x;
	float maxSampleX = pupilLUT.bounds[ LUTentry ].y;
	float minSampleY = pupilLUT.bounds[ LUTentry ].z;
	float maxSampleY = pupilLUT.bounds[ LUTentry ].w;

	float wavelength = remap( rFloat(), 0.0f, 1.0f, 380.0f, 830.0f );

	// this means you're inside the image circle, the aperture is visible
	if ( minSampleX < maxSampleX && minSampleY < maxSampleY ) {

		// generate a point in the sampling space, and calculate the corresponding ray
		vec2 samplePoint = getWeylOffset( GlobalData.frameNumber % 1024 );
		samplePoint.x = remap( samplePoint.x, 0.0f, 1.0f, minSampleX, maxSampleX );
		samplePoint.y = remap( samplePoint.y, 0.0f, 1.0f, minSampleY, maxSampleY );

		mat2 pixelRot = Rotate2D( atan( pixelUV.x, pixelUV.y ) );
		vec2 adjustedSamplePos = pixelRot * samplePoint;

		vec3 rayOrigin = vec3( uv, -lens.params.totalSystemThickness + lens.params.focusAdjust );
		vec3 rayDirection = normalize( vec3( adjustedSamplePos * lens.params.rearElementSA, -lens.params.rearElementZ ) - rayOrigin );

		// trace the specified ray through the system
		int j = 0;
		for ( j = 0; j < lens.params.numElements; j++ ) {

			float radius = lens.params.elements[ j ].radius;
			float axisPosition = lens.params.elements[ j ].axisPos - radius;
			float cosTerm = lens.params.elements[ j ].cosTerm;
			vec2 materialBack = vec2( lens.params.elements[ j ].backCauchyA, lens.params.elements[ j ].backCauchyB );
			vec2 materialFront = vec2( lens.params.elements[ j ].frontCauchyA, lens.params.elements[ j ].frontCauchyB );

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
				// this ray has not found a valid intersection
				break;
			} else {
				// continuing, prep for next iteration
				rayOrigin = rayOrigin + t * rayDirection;
				rayDirection = refract( rayDirection, normal, evaluateCauchy( materialBack.x, materialBack.y, wavelength ) / evaluateCauchy( materialFront.x, materialFront.y, wavelength ) );
			}
		}

		if ( j == lens.params.numElements ) {
			color.rgb = vec3( 1.0f );
		}
	}

	imageStore( image, ivec2( gl_GlobalInvocationID.xy ), color );
}