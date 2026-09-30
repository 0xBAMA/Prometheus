#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "common.h"
#include "srgbConvertMini.h"
//=============================================================================================================================
#define SH_CHANNEL_COUNT 16
struct GaussianSplatPacked {
	vec4 centroidOpacity;
	vec4 harmonics[ SH_CHANNEL_COUNT ];
	vec4 scale;
	vec4 rotation;
};
//=============================================================================================================================
layout ( set = 0, binding = 1 ) readonly buffer indexBuffer {
	uint indices[];
};
//=============================================================================================================================
layout ( set = 0, binding = 2, scalar ) readonly buffer vertexBuffer {
	GaussianSplatPacked splatVertices[];
};
//=============================================================================================================================
struct splatsConfig_t {
	uint indexOrderSelect; // picking between the 48 sets of indices
	uint numSplats;
	mat4 viewMatrix;
	mat4 projMatrix;
	ivec2 splatFramebufferSize;
	vec3 cameraPosition;
};
//=============================================================================================================================
layout ( set = 0, binding = 3, scalar ) uniform configBuffer {
	splatsConfig_t splatConfig;
};
//=============================================================================================================================
layout ( location = 0 ) out vec2 splatCoord;
layout ( location = 1 ) out vec3 splatColor;
layout ( location = 2 ) out float splatOpacity;
layout ( location = 3 ) out float splatDepth;
//=============================================================================================================================
const float SH_C0 = 0.28209479177387814;
const float SH_C1 = 0.4886025119029199;
const float SH_C2_0 = 1.0925484305920792;
const float SH_C2_1 = -1.0925484305920792;
const float SH_C2_2 = 0.31539156525252005;
const float SH_C2_3 = -1.0925484305920792;
const float SH_C2_4 = 0.5462742152960396;
const float SH_C3_0 = -0.5900435899266435;
const float SH_C3_1 = 2.890611442640554;
const float SH_C3_2 = -0.4570457994644658;
const float SH_C3_3 = 0.3731763325901154;
const float SH_C3_4 = -0.4570457994644658;
const float SH_C3_5 = 1.445305721320277;
const float SH_C3_6 = -0.5900435899266435;
//=============================================================================================================================
vec3 SHCoeff( uint index, int coefficient) {
	return splatVertices[ index ].harmonics[ coefficient ].xyz;
}
//=============================================================================================================================
vec3 sphericalHarmonicsToRgb(uint index, vec3 direction) {
	float x = direction.x;
	float y = direction.y;
	float z = direction.z;

	vec3 rgb = SH_C0 * SHCoeff( index, 0 );

	rgb += -SH_C1 * y * SHCoeff( index, 1 );
	rgb += SH_C1 * z * SHCoeff( index, 2 );
	rgb += -SH_C1 * x * SHCoeff( index, 3 );

	rgb += SH_C2_0 * x * y * SHCoeff( index, 4 );
	rgb += SH_C2_1 * y * z * SHCoeff( index, 5 );
	rgb += SH_C2_2 * ( 2.0 * z * z - x * x - y * y ) * SHCoeff( index, 6 );
	rgb += SH_C2_3 * x * z * SHCoeff( index, 7 );
	rgb += SH_C2_4 * ( x * x - y * y ) * SHCoeff( index, 8 );

	rgb += SH_C3_0 * y * (3.0 * x * x - y * y) * SHCoeff( index, 9 );
	rgb += SH_C3_1 * x * y * z * SHCoeff( index, 10 );
	rgb += SH_C3_2 * y * (4.0 * z * z - x * x - y * y) * SHCoeff( index, 11 );
	rgb += SH_C3_3 * z * (2.0 * z * z - 3.0 * x * x - 3.0 * y * y) * SHCoeff( index, 12 );
	rgb += SH_C3_4 * x * (4.0 * z * z - x * x - y * y) * SHCoeff( index, 13 );
	rgb += SH_C3_5 * z * (x * x - y * y) * SHCoeff( index, 14 );
	rgb += SH_C3_6 * x * (x * x - 3.0 * y * y) * SHCoeff( index, 15 );

	return clamp( rgb + vec3( 0.5f ), 0.0f, 1.0f );
}

mat3 quaternionToMat3(vec4 quaternion) {
	vec4 q = normalize(quaternion);
	float w = q.x;
	float x = q.y;
	float y = q.z;
	float z = q.w;

	return mat3(
	1.0 - 2.0 * y * y - 2.0 * z * z,
	2.0 * x * y + 2.0 * w * z,
	2.0 * x * z - 2.0 * w * y,
	2.0 * x * y - 2.0 * w * z,
	1.0 - 2.0 * x * x - 2.0 * z * z,
	2.0 * y * z + 2.0 * w * x,
	2.0 * x * z + 2.0 * w * y,
	2.0 * y * z - 2.0 * w * x,
	1.0 - 2.0 * x * x - 2.0 * y * y);
}

const mat4 vulkan_clip = mat4(
	1.0f,  0.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f, 0.0f,
	0.0f,  0.0f, 0.5f, 0.0f,
	0.0f,  0.0f, 0.5f, 1.0f
);

mat3 buildCovariance3D( vec3 scale, vec4 rotation ) {
	vec3 sigma = exp(scale);
	mat3 rotationMatrix = quaternionToMat3(rotation);
	mat3 scaleMatrix = mat3(
	sigma.x, 0.0, 0.0,
	0.0, sigma.y, 0.0,
	0.0, 0.0, sigma.z );
	mat3 rs = rotationMatrix * scaleMatrix;
	mat3 covariance = rs * transpose(rs);

	// Match the CPU-side preview flip applied to centroids in main.cpp.
	mat3 previewFlipY = mat3(
	1.0, 0.0, 0.0,
	0.0, -1.0, 0.0,
	0.0, 0.0, 1.0);
	return previewFlipY * covariance * previewFlipY;
//	return covariance;
}

mat3 projectCovarianceToScreen( vec3 centerCamera, mat3 covariance3D ) {
	float focalX = splatConfig.projMatrix[0][0] * splatConfig.splatFramebufferSize.x * 0.5;
	float focalY = splatConfig.projMatrix[1][1] * splatConfig.splatFramebufferSize.y * 0.5;
	float z = centerCamera.z;
//	float z = remap( centerCamera.z, 1.0f, 0.0f, 1.0f, -1.0f );

	mat3 jacobian = mat3(
	-focalX / z, 0.0, 0.0,
	0.0, -focalY / z, 0.0,
	focalX * centerCamera.x / (z * z), focalY * centerCamera.y / (z * z), 0.0f );

	mat3 viewLinear = mat3( splatConfig.viewMatrix );
	mat3 transform = jacobian * viewLinear;
	return transform * covariance3D * transpose(transform);
}

//=============================================================================================================================
void main () {
	// picking a splat, one per quad (2x3 verts)
	uint drawOrderOffset = gl_VertexIndex / 6;
	uint splatSelect = indices[ splatConfig.numSplats * splatConfig.indexOrderSelect + drawOrderOffset ];

	// picking a vertex, based on the current odering
	 GaussianSplatPacked mySplat = splatVertices[ splatSelect ];

	// quad verts... this is a weird scheme they're using, it flips per quad
	vec2 quadCorners[] = vec2[](
		vec2(  1.0f, -1.0f ),
		vec2( -1.0f, -1.0f ),
		vec2(  1.0f,  1.0f ),

		vec2( -1.0f, -1.0f ),
		vec2(  1.0f,  1.0f ),
		vec2( -1.0f,  1.0f )
	);
	vec2 quadCorner = quadCorners[ gl_VertexIndex % 6 ].yx;

	vec4 centerClip = splatConfig.projMatrix * splatConfig.viewMatrix * vec4( mySplat.centroidOpacity.xyz, 1.0f );
	vec3 centerCamera = vec3( splatConfig.viewMatrix * vec4( mySplat.centroidOpacity.xyz, 1.0 ) );
//	centerCamera.z = -( centerCamera.z - 1.0f ) / 2.0f;
//	centerCamera.z = remap( centerCamera.z, 1.0f, 0.0f, 1.0f, -1.0f );

	 if ( centerClip.w <= 0.0 || centerCamera.z >= 0.5f ) {
//	if ( centerClip.w <= 0.0 || centerCamera.z <= 0.001 ) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		splatCoord = quadCorner;
		splatColor = vec3(0.0);
		splatOpacity = 0.0;
		return;
	}

	mat3 covariance2D = projectCovarianceToScreen(centerCamera, buildCovariance3D( mySplat.scale.xyz, mySplat.rotation ));

	float a = covariance2D[0][0] + 0.3;
	float b = covariance2D[1][0];
	float d = covariance2D[1][1] + 0.3;

	float determinant = a * d - b * b;
	float mid = 0.5 * (a + d);
	float radius = sqrt(max(mid * mid - determinant, 0.0));
	float lambda1 = max(mid + radius, 0.01);
	float lambda2 = max(mid - radius, 0.01);

	vec2 axisDirection1 = abs(b) > 0.00001 ? normalize(vec2(b, lambda1 - a)) : (a >= d ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
	vec2 axisDirection2 = vec2(-axisDirection1.y, axisDirection1.x);

	vec2 axis1 = 3.0f * sqrt(lambda1) * axisDirection1;
	vec2 axis2 = 3.0f * sqrt(lambda2) * axisDirection2;

	vec3 ndc = centerClip.xyz / centerClip.w;
	vec2 pixelOffset = quadCorner.x * axis1 + quadCorner.y * axis2;
	vec2 ndcOffset = pixelOffset / (splatConfig.splatFramebufferSize * 0.5);

	gl_Position = vec4( ndc.xy + ndcOffset, ndc.z, 1.0f );
//	gl_Position.z = -( gl_Position.z - 1.0f ) / 2.0f;
	splatDepth = gl_Position.z;
	splatCoord = quadCorner;
	splatColor = srgb_to_rgb( sphericalHarmonicsToRgb( splatSelect,  -normalize( mySplat.centroidOpacity.xyz - splatConfig.cameraPosition ) ) );
	splatOpacity = 1.0 / (1.0 + exp( -mySplat.centroidOpacity.a ) );

//	float z = gl_Position.z;
//	splatColor = vec3(
//		z < 0.0 ? 1.0 : 0.0,
//		abs(z) / 10.0,
//		0.0
//	);
}