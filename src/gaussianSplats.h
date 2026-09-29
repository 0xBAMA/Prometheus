#pragma once

#ifndef GAUSSIANSPLATS_H
#define GAUSSIANSPLATS_H

// based on https://bfeldman.me/3dgs-weekend/

// for PLY loading
#include "third_party/happly/happly.h"
#include "engine.h"

// for GLM vector types
#include <glm/common.hpp>

// STL
#include <array>
#include <vector>

static constexpr int SH_COUNT = 16;
static constexpr int SH_CHANNEL_COUNT = 3;
static constexpr int SH_FLOAT_COUNT = SH_COUNT * SH_CHANNEL_COUNT;

#include "camera.h"

struct GaussianSplatPacked {
	glm::vec4 centroidOpacity = glm::vec4( 0.0f );
	glm::vec4 harmonics[ SH_COUNT ] = { glm::vec4( 0.0f ) };
	glm::vec4 scale = glm::vec4( 1.0f );
	glm::vec4 rotation = glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f );
};

// for precomputing index buffers with the orderings
// from iq https://iquilezles.org/articles/volumesort/
inline int calcOrder ( const glm::vec3 & dir ) {
	int signs;
	const int   sx = dir.x < 0.0f;
	const int   sy = dir.y < 0.0f;
	const int   sz = dir.z < 0.0f;
	const float ax = fabsf( dir.x );
	const float ay = fabsf( dir.y );
	const float az = fabsf( dir.z );

	if ( ax > ay && ax > az ) {
		if ( ay > az ) signs =  0 + ( ( sx << 2 ) | ( sy << 1 ) | sz );
		else           signs =  8 + ( ( sx << 2 ) | ( sz << 1 ) | sy );
	} else if( ay > az ) {
		if ( ax > az ) signs = 16 + ( ( sy << 2 ) | ( sx << 1 ) | sz );
		else           signs = 24 + ( ( sy << 2 ) | ( sz << 1 ) | sx );
	} else {
		if ( ax > ay ) signs = 32 + ( ( sz << 2 ) | ( sx << 1 ) | sy );
		else           signs = 40 + ( ( sz << 2 ) | ( sy << 1 ) | sx );
	}

	return signs;
}

inline glm::vec3 genRandomVector () {
	static thread_local std::mt19937 seedRNG( [] {
		std::random_device rd;
		std::seed_seq seq{  rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
		return std::mt19937( seq );
	} () );

	// float x = std::uniform_real_distribution< float >( min, max )( seedRNG );
	float z = std::uniform_real_distribution< float >( -1.0f, 1.0f )( seedRNG );
	float a = std::uniform_real_distribution< float >( 0.0f, 2.0f * 3.14159263f )( seedRNG );
	float r = sqrt( 1.0f - z * z );
	float x = r * cos( a );
	float y = r * sin( a );

	return glm::vec3( x, y, z );
}

struct Scene {
	std::vector<GaussianSplatPacked> splats;
	std::vector< uint32_t > indices;

	vec3 center;
	float radius;

	void ComputeSceneBounds () {
		glm::vec3 sum( 0.0f );

		for ( const auto& splat : splats )
			sum += splat.centroidOpacity.xyz();

		center = sum / static_cast< float >( splats.size() );

		std::vector< float > distances;
		distances.reserve( splats.size() );
		for (const auto& splat : splats )
			distances.push_back(glm::length( splat.centroidOpacity.xyz() - center ) );

		// establishes a reasonable threshold for the radius
		std::size_t index = static_cast< std::size_t >( static_cast< float >( distances.size() - 1 ) * 0.95f );
		std::nth_element( distances.begin(), distances.begin() + index, distances.end() );
		radius = std::max( distances[ index ] * 1.5f, 1.0f );

		fmt::print( "\nSolved scene bounds: \n" );
		fmt::print( " center: {}\n", glm::to_string( center ) );
		fmt::print( " radius: {}\n\n", radius );
	}

	// assumes list of splats has been populated
	void ComputeDistinctOrderings () {

		uint32_t numSplats = splats.size();
		indices.resize( 48 * numSplats, 0 );

		for ( int i = 0; i < 48; i++ ) {
			// initial orderings for each set of indices
			std::iota( indices.begin() + i * numSplats, indices.begin() + ( i + 1 ) * numSplats - 1, 0 );
		}

		// we need to compute 48 views... there are probably ways to do this that aren't absurd like this
		uint32_t numViewsEncountered = 0;
		bool encounteredViews[ 48 ] = { false };

		// scratch memory for the orderings
		std::vector< float > projectedPos;
		projectedPos.resize( numSplats );

		while ( numViewsEncountered < 48 ) {
			// generate a random vector
			vec3 sortVector = genRandomVector();
			int sortVectorIndex = calcOrder( sortVector );

			// see if we have hit it yet...
			if ( encounteredViews[ sortVectorIndex ] ) {
				continue;
			} else {
				// if we had not, now we have
				encounteredViews[ sortVectorIndex ] = true;
				numViewsEncountered++;

				fmt::print( "\r      {} / 48          ", numViewsEncountered );

				// precompute a projected position along the vector representing the view vector
				for ( int i = 0; i < numSplats; i++ ) {
					projectedPos[ i ] = dot( sortVector, splats[ i ].centroidOpacity.xyz() );
				}

				// initially the ordering is the identity
					// and we have a vector that we can sort with
				std::sort(indices.begin() + sortVectorIndex * numSplats,
					indices.begin() + ( sortVectorIndex + 1 ) * numSplats - 1,
					[&]( int a, int b ) {
						return projectedPos[ a ] > projectedPos[ b ];
					} );
			}
		}

		fmt::print( "\r Complete.                 " );
	}
};



inline Scene loadScene () {
	fmt::print( "Loading scene... " );
	happly::PLYData plyData( "../scene.ply" );

	Scene scene;

	// allocate memory for the splats
	const size_t nSplats = plyData.getElement( "vertex" ).count;
	scene.splats.resize( nSplats );
	fmt::print( "{} splats\n", nSplats );

	{ // positions
		std::vector< float > vertexX = plyData.getElement( "vertex" ).getProperty< float >( "x" );
		std::vector< float > vertexY = plyData.getElement( "vertex" ).getProperty< float >( "y" );
		std::vector< float > vertexZ = plyData.getElement( "vertex" ).getProperty< float >( "z" );
		for ( int i = 0 ; i < nSplats ; i++ ) {
			scene.splats[ i ].centroidOpacity.x = vertexX[ i ];
			scene.splats[ i ].centroidOpacity.y = vertexY[ i ];
			scene.splats[ i ].centroidOpacity.z = vertexZ[ i ];
		}
	}

	{ // rotations
		std::vector< float > vertexrot0 = plyData.getElement( "vertex" ).getProperty< float >( "rot_0" );
		std::vector< float > vertexrot1 = plyData.getElement( "vertex" ).getProperty< float >( "rot_1" );
		std::vector< float > vertexrot2 = plyData.getElement( "vertex" ).getProperty< float >( "rot_2" );
		std::vector< float > vertexrot3 = plyData.getElement( "vertex" ).getProperty< float >( "rot_3" );
		for ( int i = 0 ; i < nSplats ; i++ ) {
			scene.splats[ i ].rotation = { vertexrot0[ i ], vertexrot1[ i ], vertexrot2[ i ], vertexrot3[ i ] };
		}
	}

	{ // scale and opacity
		std::vector< float > vertexscale0 = plyData.getElement( "vertex" ).getProperty< float >( "scale_0" );
		std::vector< float > vertexscale1 = plyData.getElement( "vertex" ).getProperty< float >( "scale_1" );
		std::vector< float > vertexscale2 = plyData.getElement( "vertex" ).getProperty< float >( "scale_2" );
		std::vector< float > vertexopacity = plyData.getElement( "vertex" ).getProperty< float >( "opacity" );
		for ( int i = 0 ; i < nSplats ; i++ ) {
			scene.splats[ i ].scale.x = vertexscale0[ i ];
			scene.splats[ i ].scale.y = vertexscale1[ i ];
			scene.splats[ i ].scale.z = vertexscale2[ i ];
			scene.splats[ i ].centroidOpacity.a = vertexopacity[ i ];
		}
	}

	{ // spherical harmonics data is slightly more work...
		// first there are 3 f_dc_ channels containing the DC components
		std::vector< float > vertex_f_dc_0 = plyData.getElement( "vertex" ).getProperty< float >( "f_dc_0" );
		std::vector< float > vertex_f_dc_1 = plyData.getElement( "vertex" ).getProperty< float >( "f_dc_1" );
		std::vector< float > vertex_f_dc_2 = plyData.getElement( "vertex" ).getProperty< float >( "f_dc_2" );
		for ( int i = 0 ; i < nSplats ; i++ ) {
			scene.splats[ i ].harmonics[ 0 ].r = vertex_f_dc_0[ i ];
			scene.splats[ i ].harmonics[ 0 ].g = vertex_f_dc_1[ i ];
			scene.splats[ i ].harmonics[ 0 ].b = vertex_f_dc_2[ i ];
			scene.splats[ i ].harmonics[ 0 ].a = 1.0f;
		}

		// then there's N of these other channels with the harmonics data
		// find how many channels start with "f_rest_", and use that to guide the rest of the loading
		int idx = 0;
		for ( int i = 0; i < plyData.getElement( "vertex" ).getPropertyNames().size(); i++ ) {
			if ( plyData.getElement( "vertex" ).getPropertyNames()[ i ].starts_with( "f_rest_" ) ) {
				std::vector< float > vertex_f_rest_N = plyData.getElement( "vertex" ).getProperty< float >( plyData.getElement( "vertex" ).getPropertyNames()[ i ] );
				for ( int i = 0; i < nSplats ; i++ ) {
					switch ( idx % 3 ) {
					case 0: scene.splats[ i ].harmonics[ 1 + idx / 3 ].r = vertex_f_rest_N[ i ]; break;
					case 1: scene.splats[ i ].harmonics[ 1 + idx / 3 ].g = vertex_f_rest_N[ i ]; break;
					case 2: scene.splats[ i ].harmonics[ 1 + idx / 3 ].b = vertex_f_rest_N[ i ]; break;
					}
				}
				idx++;
			}
			if ( idx > ( 16 * 3 ) ) break; // tossing higher frequency components, if they exist in the file
		}
	}
	return scene;
}

#endif