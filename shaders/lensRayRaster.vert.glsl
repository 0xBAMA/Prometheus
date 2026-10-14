#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
#include "common.h"
#include "XYZSpectrum.h"
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 1, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;
//=============================================================================================================================
layout ( set = 0, binding = 2, scalar ) readonly buffer lensSegmentScratch {
	float wavelengths[ 2048 ]; // to match invocation count
	vec2 pathSegments[ 2048 * 128 ];
} scratch;
//=============================================================================================================================
layout ( location = 0 ) out vec3 color;
//=============================================================================================================================
void main () {
	// this is drawing the set of segments solved by Lens Ray Sample
	const int segmentID = ( gl_VertexIndex );
	const int pathID = segmentID / 128;

	// determine wavelength from the color
	const float wavelength = scratch.wavelengths[ pathID ];
	color = wl_rgb( wavelength );

	// placing the path segment -> look at the corresponding point in the buffer
	const vec2 myLoc = scratch.pathSegments[ segmentID ];

	// points are coming in, in an arbitrary lens space - we have the parameterization which describes that space:
	float maxDim = max( 2.0f * lens.params.maxSemiAperture, lens.params.totalSystemThickness );
	gl_Position = vec4( // overall scaling is based on 1.2x the max dimension, to give some margin -> map to 512x512 target
		remap( myLoc.x, 0.1f * maxDim, -1.1 * maxDim, -1.0f, 1.0f ),
		remap( myLoc.y, 0.6f * maxDim, -0.6f * maxDim, -1.0f, 1.0f ),
		0.5f, 1.0f
	);
}