#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "common.h"

layout ( location = 0 ) in flat vec3 colorRGB;

layout ( location = 0 ) out vec4 outFragColor;

void main () {
	outFragColor = vec4( colorRGB, 1.0f );
}