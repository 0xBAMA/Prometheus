#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

layout ( local_size_x = 8, local_size_y = 8 ) in;

#include "common.h"
#include "random.h"

// line segment distance
// https://www.shadertoy.com/view/3tdSDj
float sdSegment ( in vec2 p, in vec2 a, in vec2 b ) {
	float r = 0.0f;
	vec2 ba = b - a;
	vec2 pa = p - a;
	float h = clamp( dot( pa, ba ) / dot( ba, ba ), 0.0f, 1.0f );
	return length( pa - h * ba ) - r;
}

// circular arc distance
// https://www.shadertoy.com/view/3cXSRf
float sdArc ( vec2 p, vec2 c, float r, float a0, float a1 ) {
	vec2 d = p - c;
	float ang = atan( d.y, d.x );// + 3.1415926535f;

	// Assumes a0 < a1 and no wraparound through ±π
	if ( ang >= a0 && ang <= a1 || abs( a0 - a1 ) < 0.1f )
		return abs( length( d ) - r );

	vec2 e0 = c + r * vec2( cos( a0 ), sin( a0 ) );
	vec2 e1 = c + r * vec2( cos( a1 ), sin( a1 ) );

	return min( length( p - e0 ), length( p - e1 ) );
}

struct geometryStruct {
	float data[ 16 ];
};

layout( set = 0, binding = 1, std430 ) buffer geoBuffer {
	geometryStruct geoData[];
};

layout( set = 0, binding = 2, std430 ) buffer bboxBuffer {
	vec4 bboxes[];
};

layout( set = 0, binding = 3, std430 ) buffer gridCellsUncompacted {
	int gridCells[];
};

void main () {

	// determine location on the image
	uvec2 loc = gl_GlobalInvocationID.xy;

	// seeding RNG
	seed = PushConstants.wangSeed + loc.x * 6969 + loc.y * 8675309;

	// place the test point in the middle of the grid cell
	vec2 pTest = loc; // location in grid space

	// scaling the point based on the grid scalar
	const float gs = GlobalData.gridScalar;
	pTest *= gs; // convert to pixel space

	// based on half of the diagonal dimension of the grid cell... rounded up
		// if the SDF returns less than this, we need to add it to the list for this cell
	const float dThresh = 0.708f * gs;

	// grid bounds check ( we invoke per grid cell, not per pixel )
	int count = 0;
	uint baseIdx = 16 * ( loc.x + GlobalData.gridDims.x * loc.y );

	if ( loc.x < GlobalData.gridDims.x && loc.y < GlobalData.gridDims.y ) {
		// for primitives
		for ( int i = 0; i < GlobalData.numPrimitives && count < 15; i++ ) {
			// if I'm in the bbox, evaluate distance
			vec4 bbox = bboxes[ i ];
			if ( pTest.x >= bbox.x && pTest.x <= bbox.y && pTest.y >= bbox.z && pTest.y <= bbox.w ) {
				bool write = false;
				geometryStruct g = geoData[ i ];

				// handling different primitive types
				switch ( int( g.data[ 15 ] ) ) {
				case 0: { // segment
					const vec2 a = vec2( g.data[ 0 ], g.data[ 1 ] ) / GlobalData.gridScalar;
					const vec2 b = vec2( g.data[ 2 ], g.data[ 3 ] ) / GlobalData.gridScalar;
					for ( int k = 0; k < 64 && !write; k++ ) {
						const float dSeg = sdSegment( pTest + rFloat2() * gs, a, b );
						if ( dSeg < dThresh ) {
							write = true;
						}
					}
					break;
				}

				case 1: { // arc
					const vec2 center = vec2( g.data[ 0 ], g.data[ 1 ] ) / GlobalData.gridScalar;
					const float r = g.data[ 2 ] / GlobalData.gridScalar;
					const float aMin = g.data[ 3 ]; // minimum angle
					const float aMax = g.data[ 4 ]; // maximum angle
					for ( int k = 0; k < 64 && !write; k++ ) {
						const float dArc = sdArc( pTest + rFloat2(), center, r, aMin, aMax );
						if ( dArc < dThresh ) {
							write = true;
						}
					}
					break;
				}

				default:
					break;
				}

				if ( write ) {
					// I need to store this index, in the grid cell
					gridCells[ baseIdx + 1 + count ] = i;
					count++;
				}
			}
		}

		// store final count, capped at 15
		gridCells[ baseIdx ] = min( count, 15 );
	}
}
