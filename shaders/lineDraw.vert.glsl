#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "common.h"
#include "XYZSpectrum.h"

struct raySegment {
	float wavelength;
	float brightness;
	vec2 a;	// first point
	vec2 b;	// second point
};

layout( set = 0, binding = 1, std430 ) readonly buffer pointBuffer {
	raySegment rays[];
};

layout ( location = 0 ) out flat vec3 colorRGB;

void main () {
	int idx = gl_VertexIndex / 2;
	raySegment r = rays[ idx ];

	if ( r.brightness == 0.0f ) {
		// unused segments shouldn't be drawn
		gl_Position = vec4( -10000.0f );
	} else {
		gl_Position = vec4( ( gl_VertexIndex % 2 == 0 ) ? r.a : r.b, 0.5f, 1.0f );
	}
	colorRGB = wl_rgb( r.wavelength ) * r.brightness;
}