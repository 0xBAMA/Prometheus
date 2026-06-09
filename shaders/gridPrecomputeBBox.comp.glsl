#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

layout ( local_size_x = 64 ) in;

#include "common.h"

struct geometryStruct {
	float data[ 16 ];
};

layout( set = 0, binding = 1, std430 ) buffer geoBuffer {
	geometryStruct geoData[];
};

layout( set = 0, binding = 2, std430 ) buffer bboxBuffer {
	vec4 bboxes[];
};

vec4 getBBox ( geometryStruct g ) {
	vec4 bbox = vec4( 0.0f );
	switch ( int( g.data[ 15 ] ) ) {
	case 0: { // this is a line segment
		// segment mapping:
		// 0: a.x
		// 1: a.y
		// 2: b.x
		// 3: b.y
		// 4-12: unused
		// 13: material ID
		// 14: invert flag
		// 15: 0 -> line segment

		vec2 a = vec2( g.data[ 0 ], g.data[ 1 ] ) / GlobalData.gridScalar;
		vec2 b = vec2( g.data[ 2 ], g.data[ 3 ] ) / GlobalData.gridScalar;

		// computing grid space bbox
		bbox.x = floor( min( a.x, b.x ) );
		bbox.y = ceil( max( a.x, b.x ) );
		bbox.z = floor( min( a.y, b.y ) );
		bbox.w = ceil( max( a.y, b.y ) );

		break;
	}

	case 1: { // this is a circular arc bbox
	// arc mapping:
		// 0: center.x
		// 1: center.y
		// 2: radius
		// 3: thetaMin
		// 4: thetaMax
		// 5-12: unused
		// 13: material ID
		// 14: invert flag
		// 15: 1 -> circular arc

		vec2 center = vec2( g.data[ 0 ], g.data[ 1 ] ) / GlobalData.gridScalar;
		float r = g.data[ 2 ] / GlobalData.gridScalar;
		float aMin = g.data[ 3 ]; // minimum angle
		float aMax = g.data[ 4 ]; // maximum angle

		// need to locate the two initial points
		vec2 p0 = center + r * vec2( cos( aMin ), sin( aMin ) );
		vec2 p1 = center + r * vec2( cos( aMax ), sin( aMax ) );

		// initial values for a bounding box, since we know the endpoints are contained
		vec2 bMin = min( p0, p1 );
		vec2 bMax = max( p0, p1 );

		// skipping any trig
		if ( aMin <= 0.0f && aMax >= 0.0f ) {
			bMin = min( bMin, center + vec2( r, 0.0f ) );
			bMax = max( bMax, center + vec2( r, 0.0f ) );
		}

		if ( aMin <= piHalf && aMax >= piHalf ) {
			bMin = min( bMin, center + vec2( 0.0f, r ) );
			bMax = max( bMax, center + vec2( 0.0f, r ) );
		}

		if ( aMin <= pi && aMax >= pi ) {
			bMin = min( bMin, center + vec2( -r, 0.0f ) );
			bMax = max( bMax, center + vec2( -r, 0.0f ) );
		}

		if ( aMin <= 3.0f * piHalf && aMax >= 3.0f * piHalf ) {
			bMin = min( bMin, center + vec2( 0.0f, -r ) );
			bMax = max( bMax, center + vec2( 0.0f, -r ) );
		}

		const float bias = 0.25f;
		bbox.x = floor( bMin.x - bias );
		bbox.y = ceil( bMax.x + bias );
		bbox.z = floor( bMin.y - bias );
		bbox.w = ceil( bMax.y + bias );

		break;
	}

	default:
		break;
	}

	return bbox;
}

void main () {
	// 1D bounds checking based on current primitive count
	const uint idx = gl_GlobalInvocationID.x;
	if ( idx < GlobalData.numPrimitives ) {
		// load the data for this primitive
		geometryStruct g = geoData[ idx ];

		// compute the bbox
		vec4 bbox = getBBox( g );

		// store the bbox parameters to the buffer
		bboxes[ idx ] = bbox;
	}
}
