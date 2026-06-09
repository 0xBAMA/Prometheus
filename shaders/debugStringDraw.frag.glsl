#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "common.h"

layout ( location = 0 ) in flat uint stringIDX;
layout ( location = 1 ) in flat ivec2 glyphSize;

layout ( location = 0 ) out vec4 outFragColor;

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

// glyph mask from the LUT
bool getGlyphMask ( uvec2 pixel, uint pickedGlyph ) {

	// off-glyph returns false
	if ( pixel.x >= glyphSize.x || pixel.y >= glyphSize.y )
	return false;

	// invalid glyph
	if ( pickedGlyph > 255 )
	return false;

	// char -> corresponding location on the glyph LUT
	ivec2 loc = ivec2( pixel ) + glyphSize.xy * ivec2( pickedGlyph % 16, pickedGlyph / 16 );

	float glyphSample;
	switch ( StringConfig.debugStrings[ stringIDX ].debugStringFontPick ) {
		case 0: glyphSample = texelFetch( font_Codepage437, loc, 0 ).r;
		break;

		case 1: glyphSample = texelFetch( font_fatfont, loc, 0 ).r;
		break;

		case 2: glyphSample = texelFetch( font_tinyfont, loc, 0 ).r;
		break;

		default: break;
	}

	// then you can add the pixel offset and return alpha != 0
	if ( glyphSample > 0.0f )
		return true;
	else
		return false;
}

void main () {
	outFragColor = vec4( 1.0f );

	// fetch glyph id -> check glyph mask
	uvec2 myOffset = uvec2( gl_FragCoord.xy - StringConfig.debugStrings[ stringIDX ].debugStringWriteLocation );
	uvec2 pixelOffset = uvec2( myOffset.x % ( glyphSize.x + 1 ), myOffset.y );
	uint myGlyph = uint( StringConfig.debugStrings[ stringIDX ].debugStringData[ int( myOffset.x / ( glyphSize.x + 1 ) ) ] );
	bool onGlyph = getGlyphMask( pixelOffset, myGlyph );

	// invisible foreground or background possible, or both if you want
//	if ( !onGlyph && ( StringConfig.debugStringBackgroundColor.a == 0.0f ) )
//		shouldWrite = false;
//	else if ( onGlyph && ( StringConfig.debugStringFillColor.a == 0.0f ) )
//		shouldWrite = false;

	if ( !onGlyph )
		discard;
	else
		outFragColor = StringConfig.debugStrings[ stringIDX ].debugStringFillColor;
}