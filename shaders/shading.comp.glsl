#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 256, local_size_y = 1 ) in;
//=============================================================================================================================
#include "common.h"
#include "random.h"
//=============================================================================================================================
void main () {
	// this shader handles the "shading", which encompasses a couple things
		// new ray generation
		// tallying contributions to the pathtrace state

}
