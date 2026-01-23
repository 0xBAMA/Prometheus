#version 450

// interpoloated shader input
layout ( location = 0 ) in vec3 inColor;

// output write attachment
layout ( location = 0 ) out vec4 outFragColor;

void main () {
	// assign the interpolated color
	outFragColor = vec4( inColor, 1.0f );
}