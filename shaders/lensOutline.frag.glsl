#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
#include "common.h"
#include "random.h"
//=============================================================================================================================
#include "lens.h"
layout ( set = 0, binding = 1, scalar ) uniform lensParameters {
	GPULensDescription params;
} lens;

layout( set = 0, binding = 2, scalar ) readonly uniform lensOutlines {
	vec2 points[];
};

layout ( location = 0 ) in vec3 color;

layout ( location = 0 ) out vec4 outFragColor;

void main () {
	outFragColor = vec4( 2.0f * color, 1.0f );
}