#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "common.h"

struct debugLinePoint {
	vec4 position; // xy has position
	vec4 color;
};

layout( set = 0, binding = 1, std430 ) readonly buffer debugLineBuffer {
	debugLinePoint points[];
};

layout ( location = 0 ) out flat vec3 colorRGB;

void main () {
	int idx = gl_VertexIndex;

	gl_Position = vec4( remap( points[ idx ].position.x, 0.0f, GlobalData.floatBufferResolution.x, -1.0f, 1.0f ),
						remap( points[ idx ].position.y, 0.0f, GlobalData.floatBufferResolution.y, -1.0f, 1.0f ),
						points[ idx ].position.z, 1.0f );
	colorRGB = points[ idx ].color.rgb;
}