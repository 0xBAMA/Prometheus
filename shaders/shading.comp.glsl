#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 256, local_size_y = 1 ) in;
//=============================================================================================================================
#include "common.h"
#include "random.h"
#include "rayState.h"
#include "draine2.h" // phase function
#include "XYZSpectrum.h"
//=============================================================================================================================
layout ( set = 0, binding = 1 ) buffer rayBuffer {
	rayState_t rays[];
};
//=============================================================================================================================
layout ( set = 0, binding = 2 ) buffer lightTraceRayBuffer {
	rayState_t lightTraceRays[];
};
//=============================================================================================================================
// Spectral Reflectance LUT
layout ( set = 0, binding = 3 ) uniform sampler3D jakobLUT;
float sRGBtoReflectance ( vec3 sRGBColor, float lambda ) {
	vec3 coeff = texture( jakobLUT, sRGBColor ).rgb;
	float x = fma( fma( coeff.x, lambda, coeff.y ), lambda, coeff.z ),
	y = 1.0f / sqrt( fma( x, x, 1.0f ) );
	return fma( 0.5f * x, y, 0.5f );
}
//=============================================================================================================================
// List of lights
struct LightEmitterParameters {
	vec3 position;
	vec3 direction;
	float radius;
	float angleThresh;
	vec3 previewColor;
};
layout( set = 0, binding = 4, scalar ) uniform emitterParameters {
	LightEmitterParameters params[ 256 ];
} EmitterParameters;
//=============================================================================================================================
layout ( set = 0, binding = 5 ) uniform sampler2D lightPDF; // Light PDFs, spectral power distribution
layout ( set = 0, binding = 6 ) uniform sampler2D lightiCDF; // Light iCDFs, for importance sampling
layout ( set = 0, binding = 7 ) uniform usampler2D lightPick; // For picking a light, for importance sampling by brightness
//=============================================================================================================================
// Atomic Tally Textures
layout ( r32ui, set = 0, binding = 8 ) uniform uimage2D RTally;
layout ( r32ui, set = 0, binding = 9 ) uniform uimage2D GTally;
layout ( r32ui, set = 0, binding = 10 ) uniform uimage2D BTally;
layout ( r32ui, set = 0, binding = 11 ) uniform uimage2D CTally;
//=============================================================================================================================
void main () {
	uint idx = gl_GlobalInvocationID.x;
	seed = PushConstants.wangSeed + 8675309 * idx;

	// values to be written to the buffers
	rayState_t ray = rays[ idx ];
	// light trace ray will be starting fresh every time
	rayState_t lightTraceRay;
	StateReset( lightTraceRay );

	if ( !isDead( ray ) && ( GetBounce( ray ) <= GlobalData.bounces ) ) {
// this shader handles the "shading", which encompasses a couple things, and basically making
	// any required contributions to the pathtrace state based on the prior intersection

		// epsilon bump
		vec3 origin = GetRayOrigin( ray );
		origin += GetRayDirection( ray ) * GetDistance( ray ) + 3.0f * GlobalData.epsilon * GetNormal( ray );
		SetRayOrigin( ray, origin );

		{// spawning rays for the light traces, into the light trace ray buffer
			// it does not respect any existing state in the buffer, it will overwrite with a new light trace
			// the light trace will be evaluated, next time the intersect shader rus

			vec3 shadowRayOrigin = GetRayOrigin( ray );
			// picking one of the lights
			uint pickedLight = texture( lightPick, rFloat2() ).r;
			LightEmitterParameters l = EmitterParameters.params[ pickedLight ];
			// generate a point on the light
			vec2 diskOffset = CircleOffset();
			vec3 up = ( l.direction == vec3( 1.0f, 0.0f, 0.0f ) ) ? vec3( 0.0f, 1.0f, 0.0f ) : vec3( 1.0f, 0.0f, 0.0f );
			vec3 diskBasisX = cross( up, l.direction );
			vec3 diskBasisY = cross( diskBasisX, l.direction );
			vec3 pLight = l.position + l.radius * ( diskOffset.x * diskBasisX + diskOffset.y * diskBasisY );
			// this length is important, because we don't have pLight when the light trace takes place (could, but this way works too)
			vec3 shadowRayDirection = ( pLight - shadowRayOrigin );

			// hijacking length of direction vector to give light index in the buffer (must handle ==0 case, as it collapses direction)
			SetTransmission( lightTraceRay, GetTransmission( ray ) );
			SetRayOrigin( lightTraceRay, shadowRayOrigin );
			SetRayDirection( lightTraceRay, float( pickedLight + 1 ) * normalize( shadowRayDirection ) );
			SetDistance( lightTraceRay, length( shadowRayDirection ) );
			SetWavelength( lightTraceRay, GetWavelength( ray ) );
			SetPixelIndex( lightTraceRay, GetPixelIndex( ray ) );
		}

		// there is a possibility that this should happen before the light trace is spawned, but this is the order it takes place in right now
		{ // evaluating material based on intersection result
			switch ( int( GetMatIoR( ray ).x )  ) {
				// ESCAPE
				case NOHIT:
				// this ray has escaped the scene to the sky, so we take a sky sample (optionally) + kill it
				// accumulatedRadiance += transmission * max( 3.0f * dot( ray.direction, vec3( 0.0f, 0.0f, 1.0f ) ), 0.0f );
				// accumulatedRadiance += transmission * 13.0f * step( 0.8f, dot( ray.direction, vec3( 0.0f, 0.0f, -1.0f ) ) );
				SetBounce( ray, GlobalData.bounces );
				break;

				// SURFACES
				case EMISSIVE:
				// transmission *= sceneIntersection.albedo;
				AddEnergy( ray, GetTransmission( ray ) * GetRoughnessAlbedo( ray ).y );
				SetRayDirection( ray, cosWeightedRandomHemisphereDirection( GetNormal( ray ) ) );
				break;

				case DIFFUSE:
				Attenuate( ray, GetRoughnessAlbedo( ray ).y );
				SetRayDirection( ray, cosWeightedRandomHemisphereDirection( GetNormal( ray ) ) );
				break;

				case METALLIC:
				vec2 roughnessAlbedo = GetRoughnessAlbedo( ray );
				Attenuate( ray, roughnessAlbedo.y );
				SetRayDirection( ray, normalize( ( 1.0f + GlobalData.epsilon ) * GetNormal( ray ) + mix( reflect( GetRayDirection( ray ), GetNormal( ray ) ), RandomUnitVector(), roughnessAlbedo.x ) ) );
				break;

				case MIRROR:
				Attenuate( ray, GetRoughnessAlbedo( ray ).y );
				SetRayDirection( ray, reflect( GetRayDirection( ray ), GetNormal( ray ) ) );
				break;

				// VOLUMES
				case VOLUME_DENSE:
				Attenuate( ray, GetRoughnessAlbedo( ray ).y );
				SetRayDirection( ray, sampleApproxMieDirection( GetRayDirection( ray ), 15, rFloat(), rFloat(), rFloat() ) );
				break;

				case VOLUME_SPARSE:
				Attenuate( ray, GetRoughnessAlbedo( ray ).y );
				SetRayDirection( ray, sampleApproxMieDirection( GetRayDirection( ray ), 5, rFloat(), rFloat(), rFloat() ) );
				break;

				default:
				break;
			}

		}

		// applying the russian roulette compensation term
		float t = GetTransmission( ray );
		bool rrTerminate = ( rFloat() > t );
		t *= 1.0f / t;
		SetTransmission( ray, t );

		{ // there are a few ways of terminating the ray and tallying contribution
			// includes russian roulette termination, bounce == maxBounces, and transmission < threshold

			bool terminateRay =
				( rrTerminate ) ||
				( GetBounce( ray ) == GlobalData.bounces ) ||
				( GetTransmission( ray ) < 0.001f );

			// this is how the color is determined
			if ( terminateRay ) {
				Kill( ray );
				ivec2 pixel = GetPixelIndex( ray );
				vec3 color = wl_rgb( GetWavelength( ray ) ) * clamp( GetEnergyTotal( ray ), 0.0f, 100.0f );
				imageAtomicAdd( RTally, pixel, uint( color.r * 1024 ) );
				imageAtomicAdd( GTally, pixel, uint( color.g * 1024 ) );
				imageAtomicAdd( BTally, pixel, uint( color.b * 1024 ) );
				imageAtomicAdd( CTally, pixel, 1 );
			}
		}
	} else {
		// ensure the light trace will not take place, we don't want to spawn for a dead ray
		Kill( lightTraceRay );
	}

	// writeback with current values
	rays[ idx ] = ray;
	lightTraceRays[ idx ] = lightTraceRay;
}
