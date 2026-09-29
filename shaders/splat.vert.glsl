#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "common.h"
#include "srgbConvertMini.h"
//=============================================================================================================================
#define SH_CHANNEL_COUNT 16
struct GaussianSplatPacked {
	vec4 centroidOpacity;
	vec4 harmonics[ SH_CHANNEL_COUNT ];
	vec4 scale;
	vec4 rotation;
	vec4 pad;
};
//=============================================================================================================================
layout ( set = 0, binding = 1 ) readonly buffer indexBuffer {
	uint indices[];
};
//=============================================================================================================================
layout ( set = 0, binding = 2 ) readonly buffer vertexBuffer {
	GaussianSplatPacked splatVertices[];
};
//=============================================================================================================================
struct splatsConfig_t {
	uint indexOrderSelect; // picking between the 48 sets of indices
	uint numSplats;
	mat4 viewMatrix;
	mat4 projMatrix;
	ivec2 splatFramebufferSize;
};
//=============================================================================================================================
layout ( set = 0, binding = 3, scalar ) uniform configBuffer {
	splatsConfig_t splatConfig;
};
//=============================================================================================================================
layout ( location = 0 ) out vec2 splatCoord;
layout ( location = 1 ) out vec3 splatColor;
layout ( location = 2 ) out float splatOpacity;
//=============================================================================================================================
void main () {
	// picking a splat, one per quad (2x3 verts)
	uint drawOrderOffset = gl_VertexIndex / 6;
	uint splatSelect = indices[ splatConfig.numSplats * splatConfig.indexOrderSelect + drawOrderOffset ];

	// picking a vertex, based on the current odering
	GaussianSplatPacked myVertex = splatVertices[ splatSelect ];

	// quad verts... this is a weird scheme they're using, it flips per quad
	vec2 quadCorners[] = vec2[](
		vec2( -1.0f, -1.0f ),
		vec2(  1.0f, -1.0f ),
		vec2( -1.0f,  1.0f ),
		vec2(  1.0f,  1.0f )
	);
	vec2 quadCorner = quadCorners[ gl_VertexIndex % 4 ];

//	gl_Position =
}