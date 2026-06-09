#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

layout ( local_size_x = 64 ) in;

#include "common.h"
#include "random.h"
#include "hg_sdf.h"

struct raySegment {
	float wavelength;
	float brightness;
	vec2 a;	// first point
	vec2 b;	// second point
};

raySegment getDefaultSegment () {
	raySegment r;
	r.wavelength = 0.0f;
	r.brightness = 0.0f;
	r.a = r.b = vec2( 0.0f );
	return r;
}

layout( set = 0, binding = 1, std430 ) buffer rayBuffer {
	raySegment rays[];
};

layout( set = 0, binding = 2 ) uniform sampler2D iCDFBuffer;
float getWavelengthForLight( uint selectedLight ) {
	return texture( iCDFBuffer, vec2( NormalizedRandomFloat(), ( selectedLight + 0.5f ) / textureSize( iCDFBuffer, 0 ).y ) ).r;
}

layout( set = 0, binding = 3 ) uniform usampler2D pickBuffer;
uint getPickedLight() {
	return texture( pickBuffer, vec2( NormalizedRandomFloat(), NormalizedRandomFloat() ) ).r;
}

struct LightEmitterParameters {
// base emitter
	vec2 position;
	float rotation;

// angular distribution
	float angleScalar;
	float cauchyMix;

// array modifier
	int repeats;
	float emitterSpacing;
	float width;
};

layout( set = 0, binding = 4 ) uniform emitterParameters {
	LightEmitterParameters emitterParams[ 256 ];
} EmitterParameters;

// BVH buffers
layout ( set = 0, binding = 5 ) buffer geometryBuffer {
	float geometryParameters[]; // 16-float stride
};

layout ( set = 0, binding = 6 ) buffer prefixBuffer {
	uint prefixBufferValues[]; // 2-uint stride - elements in order, index, count, index, count...
};

layout ( set = 0, binding = 7 ) buffer gridBuffer {
	uint gridBufferValues[]; // variable stride, requires prefix buffer or it is soup
};

#define NOHIT						0
#define DIFFUSE						1
#define METALLIC					2
#define MIRROR						3

// air reserve value
#define AIR							5
// below this point, we have specific forms of glass
#define CAUCHY_FUSEDSILICA			6
#define CAUCHY_BOROSILICATE_BK7		7
#define CAUCHY_HARDCROWN_K5			8
#define CAUCHY_BARIUMCROWN_BaK4		9
#define CAUCHY_BARIUMFLINT_BaF10	10
#define CAUCHY_DENSEFLINT_SF10		11
// more coefficients available at https://web.archive.org/web/20151011033820/http://www.lacroixoptical.com/sites/default/files/content/LaCroix%20Dynamic%20Material%20Selection%20Data%20Tool%20vJanuary%202015.xlsm
#define SELLMEIER_BOROSILICATE_BK7	12
#define SELLMEIER_SAPPHIRE			13
#define SELLMEIER_FUSEDSILICA		14
#define SELLMEIER_MAGNESIUMFLOURIDE	15

struct intersectionResult {
// scene intersection representation etc loosely based on Daedalus
	float dist;
	float albedo;
	float IoR;
	float roughness;
	vec2 normal;
	bool frontFacing;
	int materialType;
};

intersectionResult getDefaultIntersection () {
	intersectionResult result;
	result.dist = 0.0f;
	result.albedo = 0.0f;
	result.IoR = 0.0f;
	result.roughness = 0.0f;
	result.normal = vec2( 0.0f );
	result.frontFacing = false;
	result.materialType = NOHIT;
	return result;
}

// for the values below that depend on access to the wavelength
float wavelength;

// raymarch parameters
const float epsilon = 0.03f;
const float maxDistance = 6000.0f;

// getting the wavelength-dependent IoR for materials
float evaluateCauchy ( float A, float B, float wms ) {
	return A + B / wms;
}

float evaluateSellmeier ( vec3 B, vec3 C, float wms ) {
	return sqrt( 1.0f + ( wms * B.x / ( wms - C.x ) ) + ( wms * B.y / ( wms - C.y ) ) + ( wms * B.z / ( wms - C.z ) ) );
}

// support for glass behavior
float Reflectance ( const float cosTheta, const float IoR ) {
	#if 1
	// Use Schlick's approximation for reflectance
	float r0 = ( 1.0f - IoR ) / ( 1.0f + IoR );
	r0 = r0 * r0;
	return r0 + ( 1.0f - r0 ) * pow( ( 1.0f - cosTheta ), 5.0f );
	#else
	// "Full Fresnel", from https://www.shadertoy.com/view/csfSz7
	float g = sqrt( IoR * IoR + cosTheta * cosTheta - 1.0f );
	float a = ( g - cosTheta ) / ( g + cosTheta );
	float b = ( ( g + cosTheta ) * cosTheta - 1.0f ) / ( ( g - cosTheta ) * cosTheta + 1.0f );
	return 0.5f * a * a * ( 1.0f + b * b );
	#endif
	//	another expression used here... https://www.shadertoy.com/view/wlyXzt - what's going on there?
}

float getIORForMaterial ( int material ) {
	// There are a couple ways to get IoR from wavelength
	float wavelengthMicrons = wavelength / 1000.0f;
	const float wms = wavelengthMicrons * wavelengthMicrons;

	float IoR = 0.0f;
	switch ( material ) {
		// Cauchy second order approx
		case CAUCHY_FUSEDSILICA:			IoR = evaluateCauchy( 1.4580f, 0.00354f, wms ); break;
		case CAUCHY_BOROSILICATE_BK7:		IoR = evaluateCauchy( 1.5046f, 0.00420f, wms ); break;
		case CAUCHY_HARDCROWN_K5:			IoR = evaluateCauchy( 1.5220f, 0.00459f, wms ); break;
		case CAUCHY_BARIUMCROWN_BaK4:		IoR = evaluateCauchy( 1.5690f, 0.00531f, wms ); break;
		case CAUCHY_BARIUMFLINT_BaF10:		IoR = evaluateCauchy( 1.6700f, 0.00743f, wms ); break;
		case CAUCHY_DENSEFLINT_SF10:		IoR = evaluateCauchy( 1.7280f, 0.01342f, wms ); break;
		// Sellmeier third order approx
		case SELLMEIER_BOROSILICATE_BK7:	IoR = evaluateSellmeier( vec3( 1.03961212f, 0.231792344f, 1.01046945f ), vec3( 1.01046945f, 6.00069867e-3f, 2.00179144e-2f ), wms ); break;
		case SELLMEIER_SAPPHIRE:			IoR = evaluateSellmeier( vec3( 1.43134930f, 0.650547130f, 5.34140210f ), vec3( 5.34140210f, 5.27992610e-3f, 1.42382647e-2f ), wms ); break;
		case SELLMEIER_FUSEDSILICA:			IoR = evaluateSellmeier( vec3( 0.69616630f, 0.407942600f, 0.89747940f ), vec3( 0.89747940f, 0.00467914800f, 0.01351206000f ), wms ); break;
		case SELLMEIER_MAGNESIUMFLOURIDE:	IoR = evaluateSellmeier( vec3( 0.48755108f, 0.398750310f, 2.31203530f ), vec3( 2.31203530f, 0.00188217800f, 0.00895188800f ), wms ); break;
		default: IoR = 1.0f;
	}

	return IoR;
}

bool isRefractive ( int id ) {
	return id >= CAUCHY_FUSEDSILICA;
}

bool gridBoundsCheck ( vec3 p ) {
	return ( all( greaterThanEqual( p, ivec3( 0 ) ) ) && all( lessThanEqual( p, vec3( GlobalData.gridDims, 0 ) ) ) );
}

float cross2( vec2 a, vec2 b ) { return a.x * b.y - a.y * b.x; }

intersectionResult sceneTraceBVH ( vec2 rayOrigin, vec2 rayDirection ) {
	intersectionResult result = getDefaultIntersection();

	result.dist = maxDistance;
	result.materialType = NOHIT;
	result.albedo = 0.0f;

	// DDA traversal
	// from https://www.shadertoy.com/view/7sdSzH

	vec3 hitLocation = vec3( rayOrigin / GlobalData.gridScalar, 0.0f );
	vec3 forwards = normalize( vec3( rayDirection, 0.0f ) );
	vec3 deltaDist = 1.0f / abs( forwards );
	ivec3 rayStep = ivec3( sign( forwards ) );
	bvec3 mask0 = bvec3( false );
	ivec3 mapPos0 = ivec3( floor( hitLocation + 0.0f ) );
	vec3 sideDist0 = ( sign( forwards ) * ( vec3( mapPos0 ) - hitLocation ) + ( sign( forwards ) * 0.5f ) + 0.5f ) * deltaDist;

	#define MAX_RAY_STEPS 10000
	for ( int i = 0; i < MAX_RAY_STEPS && gridBoundsCheck( mapPos0 ); i++ ) {
		// Core of https://www.shadertoy.com/view/4dX3zl Branchless Voxel Raycasting
		bvec3 mask1 = lessThanEqual( sideDist0.xyz, min( sideDist0.yzx, sideDist0.zxy ) );
		vec3 sideDist1 = sideDist0 + vec3( mask1 ) * deltaDist;
		ivec3 mapPos1 = mapPos0 + ivec3( vec3( mask1 ) ) * rayStep;

		// consider using distance to hit
		const int linearIndex = 2 * ( mapPos0.x + GlobalData.gridDims.x * mapPos0.y );
		ivec2 prefixValue = ivec2( prefixBufferValues[ linearIndex ], prefixBufferValues[ linearIndex + 1 ] );
		if ( prefixValue.y != 0 ) { // there is a nonzero count for this grid cell

			// iterate over the contents... rare that this will be more than 1, but possible
			float dClosest = maxDistance;
			for ( int i = 0; i < prefixValue.y; i++ ) {
				// we are looking at primitives starting at location 16 * prefixValue.x
				uint primitiveBaseIdx = 16u * ( gridBufferValues[ prefixValue.x + i ] );

				// we want to test against the primitive... ( + do not accept if the hit point is outside the grid cell? )
					// math is now operating in pixel space entirely (rayOrigin, rayDirection, and intersection)
				switch ( int( geometryParameters[ primitiveBaseIdx + 15 ] ) ) {
				case 0: // line segment between a and b
					{
						vec2 a = vec2( geometryParameters[ primitiveBaseIdx + 0 ], geometryParameters[ primitiveBaseIdx + 1 ] );
						vec2 b = vec2( geometryParameters[ primitiveBaseIdx + 2 ], geometryParameters[ primitiveBaseIdx + 3 ] );
						bool invertFace = ( geometryParameters[ primitiveBaseIdx + 14 ] != 0.0f );

						// edge
						vec2 edge = b - a;
						float det = cross2( rayDirection, edge );

						// reject, parallel
						bool parallel = false;
						if ( abs( det ) < 1e-9 )
							parallel = true;

						vec2 ao = a - rayOrigin;
						float t = cross2( ao, edge ) / det;
						float u = cross2( ao, rayDirection ) / det;

						// reject based on ray + segment bounds
						bool oobReject = false;
						if ( t < 0.0f || u < 0.0f || u > 1.0f  )
							oobReject = true;

						// cantidate intersection distance is now in t
						 if ( t < dClosest && t > 0.0f && !parallel && !oobReject ) {
							// update the hit for the traversal
							result.dist = dClosest = t;

							// todo material properties
							result.materialType = int( geometryParameters[ primitiveBaseIdx + 13 ] );
							result.IoR = getIORForMaterial( result.materialType );
							result.roughness = 0.0f;
							result.albedo = 0.99f;

							// determining the normal vector for the surface
							result.normal = normalize( vec2( -edge.y, edge.x ) );
							if ( dot( rayDirection, result.normal ) > 0.0f ) {
								result.normal = -result.normal;
							}

							// CW edge winding defines front side, or opposite if invert flag is set
							result.frontFacing = invertFace ? ( det < 0.0f ) : ( det > 0.0f );
						}
					}
					break;

				case 1: // circular arc, centered at p, radius r, and covering a range of theta
					{
						// the basic circle
						vec2 p = vec2( geometryParameters[ primitiveBaseIdx + 0 ], geometryParameters[ primitiveBaseIdx + 1 ] );
						float r = geometryParameters[ primitiveBaseIdx + 2 ];

						// theta range
						float lo = geometryParameters[ primitiveBaseIdx + 3 ];
						float hi = geometryParameters[ primitiveBaseIdx + 4 ];

						bool invertFace = ( geometryParameters[ primitiveBaseIdx + 14 ] != 0.0f );

						vec2 oc = rayOrigin - p;
						float b = dot( oc, rayDirection );
						float c = dot( oc, oc ) - r * r;
						float h = b * b - c;

						if ( h < 0.0f ) continue;
						h = sqrt( h );

						// Test nearer then farther root
						for ( int i = 0; i < 2; ++i ) {
							float t = ( i == 0 ) ? ( -b - h ) : ( -b + h );
							if ( t < 0.0f ) continue;

							vec2 hit = rayOrigin + rayDirection * t;
							vec2 dNorm = normalize( hit - p );

							float a = atan( dNorm.y, dNorm.x );
							if ( a < 0.0f ) a += 6.28318530718;

							bool inArc = ( lo <= hi ) ? ( a >= lo && a <= hi ) : ( a >= lo || a <= hi );
							if ( !inArc ) continue;

							if ( t < dClosest ) {
								result.dist = dClosest = t;
								result.frontFacing = invertFace ? ( dot( rayDirection, dNorm ) > 0.0f ) : ( dot( rayDirection, dNorm ) < 0.0f );
								result.normal = ( invertFace ? -1.0f : 1.0f ) * dNorm;

								// we still need a good shading normal
								if ( dot( rayDirection, result.normal ) > 0.0f ) {
									result.normal = -result.normal;
								}

								// todo material handling
								result.materialType = int( geometryParameters[ primitiveBaseIdx + 13 ] );
								result.IoR = getIORForMaterial( result.materialType );
								result.roughness = 0.0f;
								result.albedo = 0.99f;
							}
						}
					}
					break;

				// more primitives TBD

				default:
					break;
				}
			}

			// if we got a good hit in this grid cell, we're going to break
			if ( result.materialType != NOHIT ) {
				break;
			}
		}

		sideDist0 = sideDist1;
		mapPos0 = mapPos1;
	}

	// can dereference material to get surfaceType, albedo, IoR

	// and give back whatever we got
	return result;
}

void main () {
	// pixel index
	uint loc = uint( gl_GlobalInvocationID.x );
	uint baseIdx = loc * GlobalData.numBounces;

	// seeding RNG, unique per invocation
	seed = PushConstants.wangSeed + 8675309 * loc.x;

	// the raytrace process...
	vec2 rayOrigin, rayDirection;

	// picking a light...
	uint lightPick = getPickedLight();
	LightEmitterParameters params = EmitterParameters.emitterParams[ lightPick ];

	// cache rotation matrix
	const mat2 rot = Rotate2D( params.rotation );
	const vec2 subpixelJitter = vec2( NormalizedRandomFloat(), NormalizedRandomFloat() );

	// values in the buffer set origin, direction
	float pickedRepeat = 0;
	if ( params.repeats != 1 ) {
		pickedRepeat = float( floor( NormalizedRandomFloat() * params.repeats ) ) - float( params.repeats ) / 2.0f;
	}
	vec2 offset = rot * pickedRepeat * params.emitterSpacing * vec2( 1.0f, 0.0f );

	if ( lightPick == 0 ) {
		// this is the mouse light
		rayOrigin = subpixelJitter + GlobalData.mouseLoc.xy + offset + params.width * rot * vec2( NormalizedRandomFloat() - 0.5f, 0.0f );
	} else {
		rayOrigin = subpixelJitter + params.position + offset + params.width * rot * vec2( NormalizedRandomFloat() - 0.5f, 0.0f );
	}
	// direction is the same either way
	rayDirection = normalize( Rotate2D( params.rotation + params.angleScalar * ( NormalizedRandomFloat() - 0.5f ) + params.cauchyMix * rnd_disc_cauchy().x ) * vec2( 0.0f, 1.0f ) );

	// picking a wavelength...
		// importance sampled from the light
	wavelength = getWavelengthForLight( lightPick );

	// initial values... probably redundant
	float transmission = 1.0f;
	float energy = 1.0f;

	bool deadRay = false;
	for ( int i = 0; i < GlobalData.numBounces; i++ ) {
		// we only draw segments until the ray "dies"
		if ( !deadRay ) {

			// do the scene intersection
			intersectionResult result = sceneTraceBVH( rayOrigin, rayDirection );

			// add the line to the system
			raySegment r = getDefaultSegment();
			r.a = rayOrigin;
			r.a.x = remap( r.a.x, 0.0f, GlobalData.floatBufferResolution.x, -1.0f, 1.0f );
			r.a.y = remap( r.a.y, 0.0f, GlobalData.floatBufferResolution.y, -1.0f, 1.0f );

			r.b = rayOrigin + result.dist * rayDirection;
			r.b.x = remap( r.b.x, 0.0f, GlobalData.floatBufferResolution.x, -1.0f, 1.0f );
			r.b.y = remap( r.b.y, 0.0f, GlobalData.floatBufferResolution.y, -1.0f, 1.0f );

			r.brightness = energy;
			r.wavelength = wavelength;
			rays[ baseIdx + i ] = r;

			// evaluating the russian roulette termination...
			if ( NormalizedRandomFloat() > energy )
				deadRay = true;
			energy *= 1.0f / min( energy, 1.0f ); // compensation term

			if ( energy < 0.001f ) deadRay = true;

			// evaluating the albedo's effect on transmission + energy
			transmission *= result.albedo;
			energy *= result.albedo;

			// epsilon bump + update origin
			rayOrigin = rayOrigin + result.dist * rayDirection + result.normal * epsilon * 3.0f;

			// switch on material type
			switch ( result.materialType ) {
			case DIFFUSE:
				rayDirection = normalize( CircleOffset() );
				// invert if going into the surface
				if ( dot( rayDirection, result.normal ) < 0.0f ) {
					rayDirection = -rayDirection;
				}
				break;

			case METALLIC:
				// todo
				break;

			case MIRROR:
				rayDirection = reflect( rayDirection, result.normal );
				break;

				// below this point, we have to consider the IoR for the specific form of glass... because we precomputed all the
				// varying behavior already, we can just treat it uniformly, only need to consider frontface/backface for inversion
			default:
				rayOrigin -= result.normal * epsilon * 5;
				result.IoR = result.frontFacing ? ( 1.0f / result.IoR ) : ( result.IoR ); // "reverse" back to physical properties for IoR
				float cosTheta = min( dot( -normalize( rayDirection ), result.normal ), 1.0f );
				float sinTheta = sqrt( 1.0f - cosTheta * cosTheta );
				bool cannotRefract = ( result.IoR * sinTheta ) > 1.0f; // accounting for TIR effects
				if ( cannotRefract || Reflectance( cosTheta, result.IoR ) > NormalizedRandomFloat() ) {
					rayDirection = normalize( mix( reflect( normalize( rayDirection ), result.normal ), CircleOffset(), result.roughness ).xy );
				} else {
					rayDirection = normalize( mix( refract( normalize( rayDirection ), result.normal, result.IoR ), CircleOffset(), result.roughness ).xy );
				}
				break;
			}
		} else {
			// if the ray has finished tracing, we need to zero out the rest of the segment memory, so the raster process doesn't draw anything
//			rays[ baseIdx + i ] = getDefaultSegment(); -> replaced with VkCmdFillBuffer
			break;
		}
	}
}