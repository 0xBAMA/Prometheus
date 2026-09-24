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
//=============================================================================================================================
layout ( set = 0, binding = 1 ) buffer rayBuffer {
	rayState_t rays[];
};
//=============================================================================================================================
// todo: film sensitivity LUTs, traced like the light LUTs -> importance sampling basis for wavelength
//=============================================================================================================================
void main () {

	uint idx = gl_GlobalInvocationID.x;
	seed = PushConstants.wangSeed + 8675309 * idx;

	// this shader is responsible for generating the initial camera rays
		// tbd how the pixel offsets are sourced, not sure yet

	rayState_t ray;
	StateReset( ray );

	// starting uniform sampling
	const ivec2 pixel = ivec2( rFloat2() * vec2( GlobalData.presentBufferResolution.xy ) );

//=============================================================================================================================
	// initial imagespace position for camera + jitter
	vec2 uv = ( ( vec2( pixel ) + rFloatN2() ) / ( GlobalData.presentBufferResolution ) ) * 2.0f - vec2( 1.0f );

//=============================================================================================================================
	// spherical camera logic, this will be replaced
	const float aspectRatio = float( GlobalData.presentBufferResolution.x ) / float( GlobalData.presentBufferResolution.y );
	uv *= 0.4f;
	uv.y /= aspectRatio;
	uv.x -= 0.05f;
	uv = vec2( atan( uv.y, uv.x ) + 0.5f, ( length( uv ) + 0.5f ) * acos( -1.0f ) );
	vec3 baseVec = normalize( vec3( cos( uv.y ) * cos( uv.x ), sin( uv.y ), cos( uv.y ) * sin( uv.x ) ) );
	baseVec = Rotate3D( pi / 2.0f, vec3( 2.5f, 0.4f, 1.0f ) ) * baseVec; // this is to match the other camera

	vec3 direction = normalize( -baseVec.x * GlobalData.basisX + baseVec.y * GlobalData.basisY + ( 1.0f / GlobalData.FoV ) * baseVec.z * GlobalData.basisZ );
	//	vec3 direction = normalize( aspectRatio * uv.x * GlobalData.basisX + uv.y * GlobalData.basisY + ( 1.0f / GlobalData.FoV ) * GlobalData.basisZ );
	vec3 origin = GlobalData.viewerPosition;

	// uniformly sampling wavelength, to start...
	// this should be based on the film sensitivity curves
	// wavelength = mix( 380.0f, 830.0f, rFloat() );
	float wavelength = mix( 400.0f, 700.0f, rFloat() );

//=============================================================================================================================
	// key initial values:
		// direction
		// origin
		// wavelength
		// pixel index

	SetRayDirection( ray, direction );
	SetRayOrigin( ray, origin );
	SetWavelength( ray, wavelength );
	SetPixelIndex( ray, pixel );

	// put it in the buffer
	rays[ idx ] = ray;
}
