#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "common.h"

// SSBO configuration for writing a string of N characters to the image
struct stringConfig {
	vec2 debugStringWriteLocation;
	vec4 debugStringFillColor;
	vec4 debugStringBackgroundColor;
	uint debugStringFontPick;
	uint debugStringLength;
	float debugStringDepth;
	uint8_t debugStringData[ 1024 ];
};

layout( set = 0, binding = 1, scalar ) readonly buffer debugStringConfig {
	stringConfig debugStrings[];
} StringConfig;

// font LUTs -> bind all of them, and switch with an integer
layout ( set = 0, binding = 2 ) uniform sampler2D font_Codepage437;
layout ( set = 0, binding = 3 ) uniform sampler2D font_fatfont;
layout ( set = 0, binding = 4 ) uniform sampler2D font_tinyfont;

layout ( location = 0 ) out flat uint stringIDX;
layout ( location = 1 ) out flat ivec2 glyphSize;

vec2 points[ 6 ] = vec2[ 6 ](
vec2( 0.0f, 0.0f ),
vec2( 1.0f, 0.0f ),
vec2( 1.0f, 1.0f ),

vec2( 0.0f, 0.0f ),
vec2( 0.0f, 1.0f ),
vec2( 1.0f, 1.0f )
);

void main () {
	stringIDX = gl_VertexIndex / 6;
	int vert = gl_VertexIndex % 6;

	// populate the glyph size in global scope
	switch ( StringConfig.debugStrings[ stringIDX ].debugStringFontPick ) {
		case 0: glyphSize = textureSize( font_Codepage437, 0 ) / ivec2( 16 );
		break;

		case 1: glyphSize = textureSize( font_fatfont, 0 ) / ivec2( 16 );
		break;

		case 2: glyphSize = textureSize( font_tinyfont, 0 ) / ivec2( 16 );
		break;

		default: break;
	}

	// scaling the quad -> placement is in units of pixels
		// adding 1px padding for spacing between glyphs
	vec2 p = StringConfig.debugStrings[ stringIDX ].debugStringWriteLocation
	+ points[ vert ] * ivec2( StringConfig.debugStrings[ stringIDX ].debugStringLength, 1 ) * ( ivec2( 1 ) + glyphSize );

	// positioning in NDC
	p.x = remap( floor( p.x ), 0.0f, GlobalData.floatBufferResolution.x, -1.0f, 1.0f );
	p.y = remap( floor( p.y ), 0.0f, GlobalData.floatBufferResolution.y, -1.0f, 1.0f );
	gl_Position = vec4( p.xy, StringConfig.debugStrings[ stringIDX ].debugStringDepth, 1.0f );
}
