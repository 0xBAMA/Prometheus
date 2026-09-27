#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "common.h"
//=============================================================================================================================
layout ( location = 0 ) in vec2 splatCoord;
layout ( location = 1 ) in vec3 splatColor;
layout ( location = 2 ) in float splatOpacity;
//=============================================================================================================================
layout ( location = 0 ) out vec4 outFragColor;
//=============================================================================================================================
void main () {
	outFragColor = vec4( 1.0f );
}