//=========================================================
// push constants block - updated at smallest scope
layout( push_constant ) uniform constants {
// RNG seeding from the CPU
	uint wangSeed;
} PushConstants;

#extension GL_EXT_scalar_block_layout  : require  // for packing in the UBO
#extension GL_EXT_shader_8bit_storage  : require  // for 8 and 16 bit types
#extension GL_EXT_shader_16bit_storage : require

//=========================================================
// Global config etc data in a UBO
layout( set = 0, binding = 0, scalar ) uniform globalData {
	// buffer resolutions:
	uvec2 floatBufferResolution;
	uvec2 presentBufferResolution;

	vec4 mouseLoc;

	int numRays;
	int numBounces;

	int frameNumber;
	int reset;
	int framesSinceReset;

	float brightnessScalar;
	float resolutionScalar;

	float gridScalar;
	ivec2 gridDims;

	// for mapping into the geometry buffer
	uint numPrimitives;
	uint maxPrimitives;

	// nsight layout: vec2u; vec2u; vec4; int; int; int; int; int; float; float; float; vec2i; uint; uint;
} GlobalData;
//=========================================================

#ifndef saturate
#define saturate(x) clamp(x, 0, 1)
#endif

#ifndef UINT_MAX
#define UINT_MAX (0xFFFFFFFF-1)
#endif

#ifndef PI_DEFINED
#define PI_DEFINED
const float pi = 3.1415926535f;
const float piHalf = pi / 2.0f;
const float tau = 2.0f * pi;
const float sqrtpi = 1.7724538509f;
#endif

#ifndef REMAP_DEFINED
#define REMAP_DEFINED
float remap ( float value, float inLow, float inHigh, float outLow, float outHigh ) {
	return outLow + ( outHigh - outLow ) * ( value - inLow ) / ( inHigh - inLow );
}
float remapClamp ( float value, float inLow, float inHigh, float outLow, float outHigh ) {
	return outLow + ( outHigh - outLow ) * saturate( ( value - inLow ) / ( inHigh - inLow ) );
}
float remapWrap ( float value, float inLow, float inHigh, float outLow, float outHigh ) {
	return outLow + ( outHigh - outLow ) * mod( ( value - inLow ) / ( inHigh - inLow ), 1.0f );
}
#endif

#ifndef BASIC_ROTATIONS_DEFINED
#define BASIC_ROTATIONS_DEFINED
mat2 Rotate2D ( in float a ) {
	float c = cos( a ), s = sin( a );
	return mat2( c, s, -s, c );
}
mat3 Rotate3D ( const float angle, const vec3 axis ) {
	const vec3 a = normalize( axis );
	const float s = sin( angle );
	const float c = cos( angle );
	const float r = 1.0f - c;
	return mat3(
		a.x * a.x * r + c,
		a.y * a.x * r + a.z * s,
		a.z * a.x * r - a.y * s,
		a.x * a.y * r - a.z * s,
		a.y * a.y * r + c,
		a.z * a.y * r + a.x * s,
		a.x * a.z * r + a.y * s,
		a.y * a.z * r - a.x * s,
		a.z * a.z * r + c
	);
}
#endif