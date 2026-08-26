#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

layout ( local_size_x = 16, local_size_y = 16 ) in;

#include "common.h"

// the draw image
layout ( rgba32f, set = 0, binding = 1 ) uniform image2D image;

void main () {
	imageStore( image, ivec2( gl_GlobalInvocationID.xy ), vec4( 1.0f ) );
}