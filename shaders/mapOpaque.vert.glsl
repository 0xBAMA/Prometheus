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
layout( set = 0, binding = 3, scalar ) uniform emitterParameters {
	LightEmitterParameters params[ 256 ];
} EmitterParameters;
//=============================================================================================================================
layout ( location = 0 ) out flat vec3 colorRGB;
//=============================================================================================================================
void main () {
	int idx = gl_VertexIndex;

	vec3 posInWorldspace = vec3( 0.0f );
	if ( idx < 30 ) { // this is the bbox and the cross through the origin
		if ( idx < 6 ) { // cardinal axes through the center
			posInWorldspace[ ( idx / 2 ) ] = ( idx % 2 == 0 ? -1.0f : 1.0f ) * GlobalData.sceneExtents[ ( idx / 2 ) ];
		} else {
			// there are 4 lines per axis, x, y, z
			int vert = ( idx - 6 ) % 8;
			int axis = ( ( idx - 6 ) / 8 );

			for ( int i = 0; i < 3; i++ )
				posInWorldspace[ ( axis + i ) % 3 ] = GlobalData.sceneExtents[ ( axis + i ) % 3 ] * ( ( vert % 2 == 0 ) ? -1.0f : 1.0f ), vert /= 2;
		}
		colorRGB = vec3( 0.618f );
	} else if ( idx < ( 30 + 6 ) ) { // this is the set of basis vectors representing the user's viewpoint
		int vert = ( idx - 30 ) % 2;
		int axis = ( ( idx - 30 ) / 2 ) % 3;

		posInWorldspace = GlobalData.viewerPosition;
		switch ( axis ) {
			case 0: posInWorldspace += vert * GlobalData.basisX; colorRGB = vec3( 1.0f, 0.0f, 0.0f ); break;
			case 1: posInWorldspace += vert * GlobalData.basisY; colorRGB = vec3( 0.0f, 1.0f, 0.0f ); break;
			case 2: posInWorldspace += vert * GlobalData.basisZ; colorRGB = vec3( 0.0f, 0.0f, 1.0f ); break;
			default: break;
		}

	} else { // this corresponds to one of the lights...
//		int lightIdx = ( idx - ( 18 + 18 ) ) % 16; // currently placeholder 16 verts per light, not sure yet
		posInWorldspace = vec3( -10000.0f );
	}

	// need to use the configured map resolution
	gl_Position = GlobalData.mapMatrix * vec4( posInWorldspace, 1.0f );
}