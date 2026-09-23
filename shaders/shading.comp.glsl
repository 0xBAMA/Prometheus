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
// Spectral Reflectance LUT
layout ( set = 0, binding = 2 ) uniform sampler3D jakobLUT;
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
layout( set = 0, binding = 3, scalar ) uniform emitterParameters {
	LightEmitterParameters params[ 256 ];
} EmitterParameters;
//=============================================================================================================================
layout ( set = 0, binding = 4 ) uniform sampler2D lightPDF; // Light PDFs, spectral power distribution
layout ( set = 0, binding = 5 ) uniform sampler2D lightiCDF; // Light iCDFs, for importance sampling
layout ( set = 0, binding = 6 ) uniform usampler2D lightPick; // For picking a light, for importance sampling by brightness
//=============================================================================================================================
// Atomic Tally Textures
layout ( r32ui, set = 0, binding = 7 ) uniform uimage2D RTally;
layout ( r32ui, set = 0, binding = 8 ) uniform uimage2D GTally;
layout ( r32ui, set = 0, binding = 9 ) uniform uimage2D BTally;
layout ( r32ui, set = 0, binding = 10 ) uniform uimage2D CTally;
//=============================================================================================================================
void main () {

	// this shader handles the "shading", which encompasses a couple things
		// new ray generation
		// tallying contributions to the pathtrace state

}
