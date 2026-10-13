#include "camera.h"

using glm::vec3;
using glm::vec2;

lensSystem::lensSystem ( const std::vector< interfaceDescription >& elementsSrc_in ) {

	// does the work to create the GPU data from this representation
	loadSystem( elementsSrc );
}

void lensSystem::clear () {
	// reset the resources
	lensOutlines.clear();
	elementsSrc.clear();
}

void lensSystem::loadSystem ( const std::vector< interfaceDescription >& elementsSrc_in ) {
	// caching the input list
	elementsSrc = elementsSrc_in;

	// placeholder, full frame
	lensParameters.filmSize = glm::vec2( 36.0f, 24.0f );

	// prepping some data for the lens system
	lensParameters.numElements = elementsSrc.size();

	lensParameters.maxSemiAperture = 0.0f;
	for ( auto& element : elementsSrc ) {
		lensParameters.maxSemiAperture = std::max( lensParameters.maxSemiAperture, element.semiAperture );
		lensParameters.totalSystemThickness += element.thickness;
	}

	std::vector< uint8_t > elementCodes;
	elementCodes.push_back( 0b01 ); // first element is always air-glass
	for ( int i = 1; i < elementsSrc.size(); i++ ) {
		elementCodes.push_back( ( elementsSrc[ i - 1 ].isAir ? 0 : 2 ) + ( elementsSrc[ i ].isAir ? 0 : 1 ) );
	}

	float axisPosition = 0.0f;
	for ( int i = 0; i < elementsSrc.size(); i++ ) {
		// lens parameters
		const float sa = elementsSrc[ i ].semiAperture;
		const float r = elementsSrc[ i ].radius;
		const float t = elementsSrc[ i ].thickness;

		const int iBack = elementsSrc.size() - 1 - i;
		// const vec3 color = vec3( sin( iBack - 0.95f ) / 2.0f + 0.5f, cos( iBack + 0.9f ) / 2.0f + 0.5f, sin( iBack + 0.5f ) / 2.0f + 0.5f );

		switch ( elementCodes[ i ] ) {
		case 0: // aperture stop
			lensOutlines.push_back( vec3( axisPosition, -sa - 0.33f * lensParameters.maxSemiAperture, iBack ) );
			lensOutlines.push_back( vec3( axisPosition, -sa, iBack ) );
			lensOutlines.push_back( vec3( axisPosition, sa + 0.33f * lensParameters.maxSemiAperture, iBack ) );
			lensOutlines.push_back( vec3( axisPosition, sa, iBack ) );

			break;
		default: {
			if ( r == inf ) { // plano- element
				// single line to represent
				lensOutlines.push_back( vec3( axisPosition, -sa, iBack ) );
				lensOutlines.push_back( vec3( axisPosition, sa, iBack ) );

			} else { // some curved element
				// trace an arc for the lens surface
				vec2 center = vec2( axisPosition + r, 0.0f );
				float arcAngle = std::asin( sa / abs( r ) );

				float d = glm::distance( vec2( cos( 0.0f ), sin( 0.0f ) ) * r, vec2( cos( 0.001f ), sin( 0.001f ) ) * r );
				float increment = 0.001f / d;
				for ( float k = 0.0f; k < arcAngle; k += increment ) {
					vec2 p1 = center - vec2( cos( k ), sin( k ) ) * r;
					vec2 p2 = center - vec2( cos( k + increment ), sin( k + increment ) ) * r;
					// need to clamp
					if ( abs( p2.y ) > sa ) {
						p2.y *= sa / abs( p2.y );
					}
					lensOutlines.push_back( vec3( p1, iBack ) );
					lensOutlines.push_back( vec3( p2, iBack ) );
					lensOutlines.push_back( vec3( p1.x, -p1.y, iBack ) );
					lensOutlines.push_back( vec3( p2.x, -p2.y, iBack ) );
				}
			}
			break;
		}
		}

		switch ( elementCodes[ i ] ) {
		case 1: // air-glass interface
		case 3: // cemented glass-glass interface
			vec2 p1, p2;
			if ( r == inf ) {
				// plano element, simpler to find endpoints
				p1 = vec2( axisPosition, sa );
			} else {
				vec2 center = vec2( axisPosition + r, 0.0f );
				float arcAngle = std::asin( sa / abs( r ) );
				p1 = center - vec2( cos( arcAngle ), sin( arcAngle ) ) * r;
				if ( r < 0.0f ) {
					p1.y = -p1.y;
				}
			}

			if ( elementsSrc[ i + 1 ].radius == inf ) {
				p2 = vec2( axisPosition + t, elementsSrc[ i + 1 ].semiAperture );
			} else {
				vec2 center = vec2( axisPosition + t + elementsSrc[ i + 1 ].radius, 0.0f );
				float arcAngle = std::asin(  elementsSrc[ i + 1 ].semiAperture / abs( elementsSrc[ i + 1 ].radius ) );
				p2 = center - vec2( cos( arcAngle ), sin( arcAngle ) ) * elementsSrc[ i + 1 ].radius;
				if ( elementsSrc[ i + 1 ].radius < 0.0f ) {
					p2.y = -p2.y;
				}
			}

			// edging segments
			if ( abs( p1.y - p2.y ) < 0.001f ) {
				lensOutlines.push_back( vec3( p1, -1.0f ) );
				lensOutlines.push_back( vec3( p2 , -1.0f ) );
				lensOutlines.push_back( vec3( p1.x, -p1.y, -1.0f ) );
				lensOutlines.push_back( vec3( p2.x, -p2.y, -1.0f ) );
			} else if ( p1.y > p2.y ) {
				float deltaY = p2.y - p1.y;
				float deltaX = p2.x - p1.x;
				lensOutlines.push_back( vec3( p1, -1.0f ) );
				lensOutlines.push_back(vec3( p1 + vec2( 0.0f, deltaY ), -1.0f ) );
				lensOutlines.push_back(vec3( p2 , -1.0f ) );
				lensOutlines.push_back(vec3(p2 - vec2( deltaX, 0.0f ), -1.0f ) );
				p1.y = -p1.y;
				p2.y = -p2.y;
				lensOutlines.push_back(vec3(p1, -1.0f ) );
				lensOutlines.push_back(vec3(p1 + vec2( 0.0f, -deltaY ), -1.0f ) );
				lensOutlines.push_back(vec3(p2, -1.0f ) );
				lensOutlines.push_back(vec3(p2 - vec2( deltaX, 0.0f ), -1.0f ) );
			} else {
				float deltaY = p2.y - p1.y;
				float deltaX = p2.x - p1.x;
				lensOutlines.push_back(vec3(p1, -1.0f ) );
				lensOutlines.push_back(vec3(p1 + vec2( deltaX, 0.0f ), -1.0f ) );
				lensOutlines.push_back(vec3(p2, -1.0f ));
				lensOutlines.push_back(vec3(p2 - vec2( 0.0f, deltaY ), -1.0f ) );
				p1.y = -p1.y;
				p2.y = -p2.y;
				lensOutlines.push_back(vec3(p1, -1.0f ) );
				lensOutlines.push_back(vec3(p1 + vec2( deltaX, 0.0f ), -1.0f ) );
				lensOutlines.push_back(vec3(p2, -1.0f ) );
				lensOutlines.push_back(vec3(p2 - vec2( 0.0f, -deltaY ), -1.0f ) );
			}
			break;
		default:
			break;
		}

		// bump for next arc
		axisPosition += t;
	}

	// drawing the film plane
	lensOutlines.push_back( vec3( lensParameters.totalSystemThickness, lensParameters.filmSize.y / 2.0f, -2.0f ) );
	lensOutlines.push_back( vec3( lensParameters.totalSystemThickness, -lensParameters.filmSize.y / 2.0f, -2.0f ) );

	// with a couple little bits to emphasize dimensions
	lensOutlines.push_back( vec3( lensParameters.totalSystemThickness * 0.99f, lensParameters.filmSize.y / 2.0f, -2.0f ) );
	lensOutlines.push_back( vec3( lensParameters.totalSystemThickness * 1.01f, lensParameters.filmSize.y / 2.0f, -2.0f ) );
	lensOutlines.push_back( vec3( lensParameters.totalSystemThickness * 0.99f, -lensParameters.filmSize.y / 2.0f, -2.0f ) );
	lensOutlines.push_back( vec3( lensParameters.totalSystemThickness * 1.01f, -lensParameters.filmSize.y / 2.0f, -2.0f ) );

	fmt::print( "\nElement Codes: \n" );
	for ( auto& el : elementCodes ) {
		switch ( el ) {
		case 0: fmt::print( "Aperture Stop\n" ); break;
		case 1: fmt::print( "Air-Glass Interface\n" ); break;
		case 2: fmt::print( "Glass-Air Interface\n" ); break;
		case 3: fmt::print( "Cemented Glass-Glass\n" ); break;
		}
	}
	fmt::print( "\n" );

	float axisPos = -lensParameters.totalSystemThickness;
	std::vector< GPUInterfaceDescription > elements;
	for ( int i = elementsSrc.size() - 1; i >= 0; i-- ) {

		// can then also at the same time compute some additional parameters
		float r = ( elementsSrc[ i ].radius == inf ) ? 0.0f :  elementsSrc[ i ].radius;
		float sa = elementsSrc[ i ].semiAperture;
		float t = elementsSrc[ i ].thickness;

		// then also materials, tbd on front/back stuff
		float idxPrev = elementsSrc[ i ].index;
		float abbePrev = elementsSrc[ i ].abbeN;
		const glm::vec2 idxAbbePrev = IndexAbbeToCauchyAB( idxPrev, abbePrev );

		float idxNext = ( i == 0 ) ? 1.0f : elementsSrc[ i - 1 ].index;
		float abbeNext = ( i == 0 ) ? 89.3f : elementsSrc[ i - 1 ].abbeN;
		glm::vec2 idxAbbeNext = IndexAbbeToCauchyAB( idxNext, abbeNext );

		GPUInterfaceDescription interface;
		// interface.materialBack = glm::uintBitsToFloat( glm::packHalf2x16( idxAbbePrev ) );
		// interface.materialFront = glm::uintBitsToFloat( glm::packHalf2x16( idxAbbeNext ) );

		// going to simplest possible representation
		interface.frontCauchyA = idxAbbeNext.x;
		interface.frontCauchyB = idxAbbeNext.y;
		interface.backCauchyA = idxAbbePrev.x;
		interface.backCauchyB = idxAbbePrev.y;

		// fmt::print( "Back index:{}, abbe:{}, Front index:{}, abbe:{} becomes: {}, {}\n", idxPrev, abbePrev, idxNext, abbeNext, interface.materialFront, interface.materialBack );

		// vec2 matFront = glm::unpackHalf2x16( glm::floatBitsToUint( interface.materialFront ) );
		// vec2 matBack = glm::unpackHalf2x16( glm::floatBitsToUint( interface.materialBack ) );
		// fmt::print( "Back index:{}, abbe:{}, Front index:{}, abbe:{}\n\n", matFront.x, matFront.y, matFront.x, matFront.y );


		// reserve value of 0.0f is easier to check for on the GPU, rather than the infinity constant
		interface.radius = r;

		// somewhat redundant, but I want to keep it for now
		interface.semiAperture = sa;

		// bump axis position for next iteration
		axisPos += t;

		// since this behavior is uniform for both positive and negative radii, this can be handled the same for positive and negative radii
		interface.axisPos = axisPos;

		// in order to pull the trig out of the spherical cap intersector, you can precompute this term rather than doing so at runtime
			// for plano elements this is actually just encoding the semiaperture, it's redundant but it doesn't really matter right now
		interface.cosTerm = ( r == 0.0f ) ? sa : float( std::cos( std::asin( sa / abs( r ) ) ) );

		elements.push_back( interface );
	}

	// report the contents of the buffer
	fmt::print( "Specifying a lens with {} interfaces, total system thickness: {}\n", elements.size(), lensParameters.totalSystemThickness );
	int idx = 0;
	for ( auto& element : elements ) {
		lensParameters.maxSemiAperture = std::max( lensParameters.maxSemiAperture, element.semiAperture );
		// vec2 matFront = glm::unpackHalf2x16( glm::floatBitsToUint( element.materialFront ) );
		// vec2 matBack = glm::unpackHalf2x16( glm::floatBitsToUint( element.materialBack ) );
		// fmt::print( "{}: r: {}, pos: {}, sa: {}, front index: {}, abbe: {}, back index: {} abbe: {}\n", idx, element.radius, element.axisPos, element.semiAperture, matFront.x, matFront.y, matBack.x, matBack.y );
		idx++;
	}

	lensParameters.numElements = elements.size();
	for ( int i = 0; i < 32; i++ ) {
		if ( i < elements.size() ) {
			lensParameters.interfaces[ i ] = elements[ i ];
		} else {
			memset( &lensParameters.interfaces[ i ], 0.0f, sizeof( GPUInterfaceDescription ) );
		}
	}

}

