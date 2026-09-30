#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "common.h"
//=============================================================================================================================
layout ( location = 0 ) in vec2 splatCoord;
layout ( location = 1 ) in vec3 splatColor;
layout ( location = 2 ) in float splatOpacity;
layout ( location = 3 ) in float splatDepth;
//=============================================================================================================================
layout ( location = 0 ) out vec4 outFragColor;
//=============================================================================================================================
void main () {
	float power = -4.5 * dot( splatCoord, splatCoord );
	if ( power < -9.0 ) {
		discard;
	}

	float alpha = min( 0.99f, splatOpacity * exp( power ) );
	if (alpha < 0.001) {
		discard;
	}

	 outFragColor = vec4( splatColor, alpha );
//	outFragColor = vec4( splatColor, 1.0f );
//	outFragColor = vec4( mix( 1.0f, 0.1f, 1.0f - splatDepth ) * splatColor, 1.0f );
	// outFragColor = vec4( splatCoord, 0.0f, alpha );
}