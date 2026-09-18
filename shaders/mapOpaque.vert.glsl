#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "common.h"
//=============================================================================================================================
struct LightEmitterParameters {
	vec3 position;
	vec3 direction;
	float radius;
	float angleThresh;
	vec3 previewColor;
};
layout( set = 0, binding = 1, scalar ) uniform emitterParameters {
	LightEmitterParameters params[ 256 ];
} EmitterParameters;
//=============================================================================================================================
layout ( location = 0 ) out vec3 colorRGB;
layout ( location = 1 ) out float zPos;
//=============================================================================================================================
void main () {
	int idx = gl_VertexIndex;

	vec3 posInWorldspace = vec3( 0.0f );
	if ( idx < 30 ) { // this is the bbox and the cross through the origin
		if ( idx < 6 ) { // cardinal axes through the center
			posInWorldspace[ ( idx / 2 ) ] = ( idx % 2 == 0 ? -1.0f : 1.0f ) * GlobalData.sceneExtents[ ( idx / 2 ) ];
			// faint coloration of the major axis, to grey on the negative side
			if ( idx % 2 == 0 ) {
				colorRGB = vec3( 0.1618f );
			} else {
				colorRGB = vec3( 0.0f );
				colorRGB[ ( idx / 2 ) ] = 0.1618f;
			}
		} else {
			// there are 4 lines per axis, x, y, z
			int vert = ( idx - 6 ) % 8;
			int axis = ( ( idx - 6 ) / 8 );

			for ( int i = 0; i < 3; i++ )
				posInWorldspace[ ( axis + i ) % 3 ] = GlobalData.sceneExtents[ ( axis + i ) % 3 ] * ( ( vert % 2 == 0 ) ? -1.0f : 1.0f ), vert /= 2;

			// uniform grey
			colorRGB = vec3( 0.1618f );
		}
	} else if ( idx < ( 30 + 6 ) ) { // this is the set of basis vectors representing the user's viewpoint
		int vert = ( idx - 30 ) % 2;
		int axis = ( ( idx - 30 ) / 2 ) % 3;

		posInWorldspace = GlobalData.viewerPosition;
		switch ( axis ) {
			case 0: posInWorldspace += 3 * vert * GlobalData.basisX; colorRGB = vec3( 3.0f, 0.0f, 0.0f ); break;
			case 1: posInWorldspace += 3 * vert * GlobalData.basisY; colorRGB = vec3( 0.0f, 3.0f, 0.0f ); break;
			case 2: posInWorldspace += 3 * vert * GlobalData.basisZ; colorRGB = vec3( 0.0f, 0.5f, 3.0f ); break;
			default: break;
		}

	} else { // this corresponds to one of the lights...
		int vertIdx = ( idx - ( 30 + 6 ) ) % 256;
		int lightIdx = ( idx - ( 30 + 6 ) ) / 256; // currently placeholder 256 verts per light, not sure yet

		LightEmitterParameters l = EmitterParameters.params[ lightIdx ];

		if ( vertIdx < 2 ) {
		// ray along the direction of the light
			posInWorldspace = ( vertIdx == 0 ) ? l.position : l.position + 3.0f * l.direction;
			colorRGB = 2.0f * l.previewColor;
		} else {
		// ring indicating the size of the light
			int ringIdx = vertIdx - 2;

			// get an offset point... we need one point on the ring
			vec3 basePointOffset = ( cross( l.direction, ( l.direction == vec3( 1.0f, 0.0f, 0.0f ) ) ? vec3( 0.0f, 1.0f, 0.0f ) : vec3( 1.0f, 0.0f, 0.0f ) ) );

			// rotate it the appropriate amount about the light's direction vector
			posInWorldspace = l.position + l.radius * Rotate3D( ( ringIdx / 2 + ringIdx % 2 ) / 3.0f, l.direction ) * basePointOffset;
			colorRGB = vec3( 0.1618f );
		}
	}


	// need to use the configured map resolution
	gl_Position = GlobalData.mapMatrix * vec4( posInWorldspace, 1.0f );
	zPos = gl_Position.z;
}