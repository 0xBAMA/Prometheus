#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
#include "common.h"
//=============================================================================================================================
layout ( location = 0 ) in vec3 colorRGB;
layout ( location = 1 ) in float zPos;
layout ( location = 0 ) out vec4 outFragColor;
//=============================================================================================================================
void main () {
	// want to add a bit of depth coloring
	outFragColor = vec4( colorRGB * ( 0.8f * zPos + 0.2f ), 1.0f );
}