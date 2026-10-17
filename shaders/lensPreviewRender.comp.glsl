#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 16, local_size_y = 16 ) in;
//=============================================================================================================================
#include "common.h"
#include "random.h"
#include "XYZSpectrum.h"
//=============================================================================================================================
layout ( rgba32f, set = 0, binding = 1 ) uniform image2D image; // accumulator image
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 2, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;
//=============================================================================================================================
layout( set = 0, binding = 3, scalar ) buffer pupilBounds {
	vec4 bounds[ 4096 ];
} pupilLUT;
//=============================================================================================================================
layout ( set = 0, binding = 4 ) uniform sampler2D ISO_12233TestPattern;
//=============================================================================================================================
void main () {
	vec4 color = vec4( 0.0f, 0.0f, 0.0f, 1.0f );

	seed = 8675309 * gl_GlobalInvocationID.x + 42069 * gl_GlobalInvocationID.y + PushConstants.wangSeed;

	for ( int k = 0; k < 64; k++ ) {

		// determine my position on the film
//		vec2 pixelUV = ( gl_GlobalInvocationID.xy + rFloatN2() ) / GlobalData.presentBufferResolution - vec2( 0.5f );
//		pixelUV.x /= ( GlobalData.presentBufferResolution.x / GlobalData.presentBufferResolution.y );
//		pixelUV.y = -pixelUV.y;

		vec2 filmLocation = lens.params.filmSize.xy * ( ( gl_GlobalInvocationID.xy + rFloatN2() ) / GlobalData.presentBufferResolution.xy - vec2( 0.5f ) );

		int LUTentry = int( floor( remap( length( filmLocation ), 0.0f, length( lens.params.filmSize.xy / 2.0f ), 0.0f, 4096.0f ) ) );

		vec2 basisY = ( LUTentry == 0 ) ? vec2( 0.0f, 1.0f ) : normalize( filmLocation ).xy;
		vec2 basisX = vec2( basisY.y, -basisY.x );

		float minSampleX = pupilLUT.bounds[ LUTentry ].x;
		float maxSampleX = pupilLUT.bounds[ LUTentry ].y;
		float minSampleY = pupilLUT.bounds[ LUTentry ].z;
		float maxSampleY = pupilLUT.bounds[ LUTentry ].w;

		float wavelength = remap( rFloat(), 0.0f, 1.0f, 380.0f, 830.0f );

		// this means you're inside the image circle, the aperture is visible
		if ( minSampleX <= maxSampleX && minSampleY <= maxSampleY ) {

//			color.gb += vec2(abs(minSampleX - maxSampleX), abs(minSampleY - maxSampleY));

			// generate a point in the sampling space, and calculate the corresponding ray
			// vec2 samplePoint = getWeylOffset( GlobalData.frameNumber % 1024 );
			vec2 samplePoint =
				basisX * remap( rFloat(), 0.0f, 1.0f, minSampleX, maxSampleX ) +
				basisY * remap( rFloat(), 0.0f, 1.0f, minSampleY, maxSampleY );

			vec3 rayOrigin = vec3( filmLocation, -lens.params.totalSystemThickness + lens.params.focusAdjust );
			vec3 rayDirection = normalize( vec3( samplePoint * lens.params.rearElementSA, -lens.params.rearElementZ ) - rayOrigin );

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
				// trace this ray against a plane...
				const vec3 planeNormal = vec3( 0.0f, 0.0f, -1.0f );
				float tt = rayPlaneIntersect( planeNormal, vec3( 0.0f, 0.0f, lens.params.testPatternDistance ), rayOrigin, rayDirection );

				// get a 2D point on the plane
				vec2 testPatternUV = ( 0.004f * ( rayOrigin + tt * rayDirection ).xy / vec2( lens.params.testPatternScaling ) ) + vec2( 0.5f );

				// sample the test pattern at the specified point
				vec4 testPatternRead = texture( ISO_12233TestPattern, testPatternUV );

				if ( testPatternRead.r > 0.5f ) {
					color.rgb += 2.0f * wl_rgb( wavelength );
				}
			}
		} else {
//			color.rgb += vec3( 0.0f, 0.0f, 0.0f );
		}
	}

	color.rgb /= 64.0f;

	float mixFactor;
	vec4 prevColor = imageLoad( image, ivec2( gl_GlobalInvocationID.xy ) );

	color.rgb = mix( prevColor.rgb, color.rgb, 1.0f / ( prevColor.a + 1.0f ) );
	color.a = prevColor.a + 1.0f;

	if ( GlobalData.reset != 0 || lens.params.mouseReset != 0 ) {
		color.a = 1.0f;
	}

	imageStore( image, ivec2( gl_GlobalInvocationID.xy ), color );
}