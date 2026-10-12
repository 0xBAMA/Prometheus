#pragma once

#ifndef CAMERA_H
#define CAMERA_H

#include "glm/glm.hpp"

#include "glm/ext/matrix_float4x4.hpp"
#include "glm/ext/vector_float3.hpp"
#include "glm/geometric.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/packing.hpp"

using glm::vec4;

#include <fmt/core.h>

static glm::vec2 IndexAbbeToCauchyAB( float index, float abbe ) {
	// can calculate the A and B Cauchy parameters from the two values
	// ref https://wiki.luxcorerender.org/Glass_Material_IOR_and_Dispersion
	const float b = (index - 1.0f) / abbe; // B parameter is (Nd-1)/(Vd)
	const float a = index - b * 2.897f;    // A parameter is Nd-B*2.897
	return glm::vec2( a, b );
}

// to specify plano elements, of infinite radius
static constexpr float inf = std::numeric_limits<float>::max();

// parameters for one element of the lens
struct interfaceDescription {
	// geometry needs to be set for every element
	float radius = inf;
	float thickness;
	float semiAperture;

	// material can default to air
	bool  isAir = true;
	float index = 1.0f;
	float abbeN = 89.3f;

	// air element can default the index/abbe parameters, non-air elements need to specify
	interfaceDescription( float rIn, float tIn, float saIn ) : radius( rIn ), thickness( tIn ), semiAperture( saIn ), isAir( true ) {}
	interfaceDescription( float rIn, float tIn, float saIn, float indexIn, float abbeIn ) : radius( rIn ), thickness( tIn ), semiAperture( saIn ), isAir( false ), index( indexIn ), abbeN( abbeIn ) {}
};

// keeping some redundant data this time, it doesn't matter because even a complex lens system will never exceed 1kb
struct GPUInterfaceDescription {
	float radius = 0.0f;
	float axisPos = 0.0f;
	float semiAperture = 0.0f;
	float cosTerm = 0.0f; // contains a value which helps compute the intersection on the GPU faster (no runtime trig)
	float materialFront = 0.0f; // contains two half-floats with Cauchy A and B parameters
	float materialBack = 0.0f; // contains two half-floats with Cauchy A and B parameters
	float information = 0.0f; // contains some packed control bits
	float pad = 0.0f; // padding out to 8x 4 bytes
};

struct GPULensDescription {
	float totalSystemThickness = 0.0f; // this also includes the focus offset
	float numElements = 0.0f;
	glm::vec2 filmSize{ 0.0f, 0.0f };
	float maxSemiAperture = 0.0f;

	// establishing a maximum number of lens elements which can be part of a lens system
	GPUInterfaceDescription interfaces[ 32 ]{};
};

// lens prescriptions from Modern Lens Design by Warren J. Smith
const std::vector< interfaceDescription > elementsFisheye = {
	interfaceDescription(599.383f, 35.030f, 448.4f, 1.517f, 64.2f),
	interfaceDescription(235.825f, 190.161f, 234.0f),
	interfaceDescription(605.513f, 30.025f, 251.8f, 1.487f, 70.4f),
	interfaceDescription(111.094f, 120.102f, 110.1f),
	interfaceDescription(-452.384f, 10.008f, 93.5f, 1.487f, 70.4f),
	interfaceDescription(127.733f, 45.038f, 93.5f, 1.785f, 26.1f),
	interfaceDescription(462.892f, 25.021f, 93.5f),
	interfaceDescription(inf, 15.013f, 65.4f, 1.518f, 59.0f),
	interfaceDescription(inf, 36.281f, 65.5f),
	interfaceDescription(inf, 13.762f, 15.8f),
	interfaceDescription(38507.649f, 10.008f, 84.1f, 1.785f, 26.1f),
	interfaceDescription(95.081f, 110.093f, 84.1f, 1.744f, 44.7f),
	interfaceDescription(-162.638f, 130.110f, 84.1f),
	interfaceDescription(1376.167f, 20.017f, 139.0f, 1.785f, 26.1f),
	interfaceDescription(177.275f, 150.127f, 139.0f, 1.702f, 41.0f),
	interfaceDescription(-400.339f, 18.766f, 139.0f, 1.668f, 41.9f),
	interfaceDescription(-337.536f, 150.110f, 139.0f)
};

const std::vector< interfaceDescription > elementsHypergon = {
	interfaceDescription(8.570f, 2.200f, 8.5f, 1.510f, 63.5f),
	interfaceDescription(8.630f, 6.900f, 8.5f),
	interfaceDescription(8.630f, 6.900f, 2.2f),
	interfaceDescription(-8.630f, 2.200f, 8.5f, 1.510f, 63.5f),
	interfaceDescription(-8.570f, 92.925f, 8.5f)
};

const std::vector< interfaceDescription > elementsPetzval = {
	interfaceDescription(53.000f, 19.500f, 30.0f, 1.517f, 64.2f),
	interfaceDescription(-460.000f, 2.565f, 30.0f),
	interfaceDescription(-139.700f, 5.000f, 30.0f, 1.620f, 36.4f),
	interfaceDescription(240.00f, 37.050f, 26.3f),
	interfaceDescription(59.50f, 17.000f, 21.5f, 1.517f, 64.2f),
	interfaceDescription(-42.2f, 0.9400f, 21.5f),
	interfaceDescription(-38.00f, 5.000f, 21.5f, 1.620f, 36.4f),
	interfaceDescription(-161.0f, 46.646f, 21.5f)
};

const std::vector< interfaceDescription > elementsSonnar = {
	interfaceDescription(121.480f, 8.810f, 45.0f, 1.613f, 58.6f),
	interfaceDescription(310.660f, 0.5f, 45.0f),
	interfaceDescription(73.270f, 8.380f, 40.0f, 1.613f, 58.6f),
	interfaceDescription(118.550f, 0.5f, 40.0f),
	interfaceDescription(50.420f, 33.700f, 36.1f, 1.607f, 59.5f),
	interfaceDescription(-105.1600f, 3.390f, 28.2f, 1.689f, 30.6f),
	interfaceDescription(29.030f, 10.94f, 20.6f),
	interfaceDescription(inf, 12.0f, 20.1f),
	interfaceDescription(59.390f, 13.72f, 23.0f, 1.613f, 58.6f),
	interfaceDescription(-190.620f, 31.406f, 23.0f)
};

const std::vector< interfaceDescription > elementsIkuoMoriMacro = {
	interfaceDescription(66.185f, 5.530f, 29.1f, 1.755f, 52.3f),
	interfaceDescription(166.926f, 0.120f, 28.6f),
	interfaceDescription(37.47f, 12.530f, 25.0f, 1.670f, 57.3f),
	interfaceDescription(463.86f, 7.25f, 22.6f, 1.620f, 36.3f),
	interfaceDescription(23.59f, 9.0f, 14.7f),
	interfaceDescription(inf, 11.1760f, 12.3f),
	interfaceDescription(-28.6f, 1.840f, 12.4f, 1.757f, 31.8f),
	interfaceDescription(479.12f, 7.62f, 14.4f, 1.744f, 44.9f),
	interfaceDescription(-38.908f, 4.205f, 16.1f),
	interfaceDescription(228.492f, 5.530f, 21.9f, 1.794f, 45.4f),
	interfaceDescription(-82.104f, 62.819f, 22.1f)
};


// simple element
// interfaceDescription( , , , , ),
// interfaceDescription( , , ),

// aperture stop
// interfaceDescription( , , ),

// compound element
// interfaceDescription( , , , , ),
// interfaceDescription( , , , , ),
// interfaceDescription( , , , , ),
// interfaceDescription( , , ),

// Modern Optical Engineering, p.537
const std::vector< interfaceDescription > elementsDgaussNakagawa = {
	interfaceDescription( 78.186f, 8.862f, 36.4f, 1.835f, 42.7f ),
	interfaceDescription( 259.846f, 0.232f, 36.4f ),

	interfaceDescription( 42.498f, 8.784f, 31.5f, 1.808f, 40.6f ),
	interfaceDescription( 60.180f, 3.436f, 30.8f ),

	interfaceDescription( 69.416f, 2.606f, 28.7f, 1.785f, 26.1f ),
	interfaceDescription( 28.654f, 16.888f, 23.7f ),

	interfaceDescription( inf, 14.0f, 23.6f ),

	interfaceDescription( -31.582f, 2.646f, 23.5f, 1.785f, 26.1f ),
	interfaceDescription( -99.808f, 9.982f, 29.7f, 1.808f, 40.6f ),
	interfaceDescription( -52.702f, 0.192f, 29.7f ),

	interfaceDescription( -150.994f, 9.692f, 31.9f, 1.713f, 53.8f ),
	interfaceDescription( -50.290f, 0.232f, 31.9f ),

	interfaceDescription( 338.224f, 5.174f, 30.0f, 1.713f, 53.8f ),
	interfaceDescription( -186.406f, 73.796f, 30.0f )
};

// Modern Optical Engineering p.538
const std::vector<interfaceDescription> dGaussAngenieuxF1_1 = {
	interfaceDescription( 164.120f,   10.990f, 54.0f, 1.673f, 32.2f ),
	interfaceDescription( 559.280f,    0.230f, 54.0f ),
	interfaceDescription( 100.120f,   11.450f, 51.0f, 1.670f, 47.1f ),
	interfaceDescription( 213.540f,    0.230f, 51.0f ),
	interfaceDescription( 58.040f,    22.950f, 41.0f, 1.691f, 54.7f ),
	interfaceDescription( 2551.000f,   2.580f, 41.0f, 1.673f, 32.2f ),
	interfaceDescription( 32.390f,    15.660f, 27.0f ),
	interfaceDescription( inf,       15.000f, 25.5f ),
	interfaceDescription( -40.420f,    2.740f, 25.0f, 1.699f, 30.1f ),
	interfaceDescription( 192.980f,   27.920f, 36.0f, 1.620f, 60.3f ),
	interfaceDescription( -55.530f,    0.230f, 36.0f ),
	interfaceDescription( 192.980f,    7.980f, 35.0f, 1.691f, 54.7f ),
	interfaceDescription( -225.280f,   0.230f, 35.0f ),
	interfaceDescription( 175.100f,    8.480f, 35.0f, 1.691f, 54.7f ),
	interfaceDescription( -203.540f,  55.742f, 35.0f )
};

// modern optical engineering p.539
const std::vector<interfaceDescription> angulonF5_6 = {
	interfaceDescription( inf,        6.150f, 41.4f, 1.522f, 59.5f ),
	interfaceDescription( inf,        2.360f, 41.4f ),
	interfaceDescription( 110.520f,   4.140f, 34.0f, 1.487f, 70.4f ),
	interfaceDescription( 32.048f,   22.300f, 26.6f ),
	interfaceDescription( 39.402f,   16.880f, 22.3f, 1.717f, 48.0f ),
	interfaceDescription( 19.453f,   13.270f, 13.8f, 1.501f, 56.4f ),
	interfaceDescription( 157.295f,   6.260f, 10.6f ),
	interfaceDescription( inf,        1.700f, 10.3f ),
	interfaceDescription( 251.613f,  15.610f, 10.6f, 1.620f, 60.3f ),
	interfaceDescription( -20.718f,   9.560f, 13.4f, 1.689f, 31.2f ),
	interfaceDescription( -37.760f,  38.120f, 17.0f ),
	interfaceDescription( -37.760f,   3.820f, 27.6f, 1.487f, 70.4f ),
	interfaceDescription( -369.843f, 53.701f, 35.0f )
};

const std::vector<interfaceDescription> F2_8_1degHFOV_TRIPLET_TELESCOPE_OBJECTIVE = {
	interfaceDescription( 50.098f, 4.500f, 18.0f, 1.517f, 64.2f ),
	interfaceDescription( -983.420f, 0.100f, 18.0f ),
	interfaceDescription( 56.671f, 4.500f, 17.3f, 1.517f, 64.2f ),
	interfaceDescription( -171.150f, 5.571f, 17.3f ),
	interfaceDescription( -97.339f, 3.500f, 15.0f, 1.717f, 29.5f ),
	interfaceDescription( 81.454f, 75.132f, 15.0f )
};

// D.P. Feder Curved Field F/1 Lens
const std::vector<interfaceDescription> dPFederCurvedFieldF1 = {
    interfaceDescription( 163.200f, 16.270f, 50.0f, 1.513f, 60.5f ),
    interfaceDescription( inf,       0.010f, 50.0f ),
    interfaceDescription( 115.900f, 32.500f, 47.4f, 1.513f, 60.5f ),
    interfaceDescription( -134.200f, 1.220f, 43.5f ),
    interfaceDescription( -128.700f, 4.000f, 42.6f, 1.689f, 30.9f ),
    interfaceDescription( 436.000f, 40.700f, 40.2f ),
    interfaceDescription( 95.580f, 26.500f, 34.7f, 1.511f, 63.5f ),
    interfaceDescription( -85.390f, 8.100f, 34.1f, 1.689f, 30.9f ),
    interfaceDescription( -264.400f, 13.000f, 34.3f ),
    interfaceDescription( 107.400f, 19.500f, 32.7f, 1.511f, 63.5f ),
    interfaceDescription( inf,      17.041f, 30.3f )
};

// Petzval W/FF F/1.25 12deg USP2649021 / Angenieux
const std::vector<interfaceDescription> petzvalWFF125_12deg = {
    interfaceDescription( 121.410f, 10.380f, 43.2f, 1.517f, 64.2f ),
    interfaceDescription( 1600.000f, 0.860f, 43.2f ),
    interfaceDescription( 81.320f, 19.900f, 41.4f, 1.517f, 64.2f ),
    interfaceDescription( -138.430f, 2.080f, 41.4f, 1.626f, 35.7f ),
    interfaceDescription( 138.430f, 32.150f, 41.4f ),
    interfaceDescription( inf,      31.000f, 26.9f ),
    interfaceDescription( 49.310f, 14.710f, 26.0f, 1.517f, 64.2f ),
    interfaceDescription( -60.560f, 5.540f, 26.0f, 1.626f, 35.7f ),
    interfaceDescription( -332.230f, 28.300f, 26.0f ),
    interfaceDescription( -40.660f, 1.730f, 26.0f, 1.650f, 33.7f ),
    interfaceDescription( 288.100f, 1.665f, 26.0f )
};

// 2" F/1.4 16mm Projection Lens USP3255664
const std::vector<interfaceDescription> projection16mmF14 = {
    interfaceDescription( 91.500f, 13.000f, 36.5f, 1.611f, 58.8f ),
    interfaceDescription( inf,      5.570f, 36.5f ),
    interfaceDescription( -122.780f, 5.000f, 36.5f, 1.720f, 29.3f ),
    interfaceDescription( 263.200f, 0.600f, 36.5f ),
    interfaceDescription( 108.560f, 13.200f, 36.5f, 1.611f, 58.8f ),
    interfaceDescription( -340.740f, 56.720f, 36.5f ),
    interfaceDescription( 90.320f, 9.200f, 28.5f, 1.611f, 58.8f ),
    interfaceDescription( inf,       0.600f, 28.5f ),
    interfaceDescription( 58.110f, 9.600f, 28.5f, 1.611f, 58.8f ),
    interfaceDescription( 174.020f, 17.280f, 28.5f ),
    interfaceDescription( -73.400f, 2.600f, 16.5f, 1.720f, 29.3f ),
    interfaceDescription( 81.260f, 17.318f, 16.5f )
};

// Mitsuaki Horimoto F/2.8 180deg Fish Eye Lens #7
const std::vector<interfaceDescription> horimotoFishEyeF28 = {
    interfaceDescription( 417.940f, 13.086f, 160.0f, 1.713f, 53.9f ),
    interfaceDescription( 68.230f, 57.530f, 68.0f ),
    interfaceDescription( 342.800f, 9.810f, 75.0f, 1.620f, 60.3f ),
    interfaceDescription( 78.400f, 21.620f, 66.0f ),
    interfaceDescription( 96.050f, 34.600f, 64.0f, 1.669f, 45.0f ),
    interfaceDescription( -267.860f, 4.320f, 64.0f ),
    interfaceDescription( -158.680f, 6.180f, 58.0f, 1.694f, 50.8f ),
    interfaceDescription( 78.230f, 24.710f, 54.0f, 1.668f, 41.9f ),
    interfaceDescription( -346.890f, 3.090f, 54.0f ),
    interfaceDescription( inf,       9.270f, 50.0f, 1.518f, 60.2f ),
    interfaceDescription( inf,       8.530f, 50.0f ),
    interfaceDescription( inf,      10.000f, 42.7f ),
    interfaceDescription( -5285.700f, 21.010f, 44.5f, 1.497f, 81.6f ),
    interfaceDescription( -113.800f, 0.620f, 50.0f ),
    interfaceDescription( 661.220f, 30.890f, 52.0f, 1.487f, 70.2f ),
    interfaceDescription( -63.500f, 6.180f, 52.0f, 1.805f, 25.4f ),
    interfaceDescription( -127.490f, 224.054f, 56.0f )
};

// Heliar USP 716035/1902 Harting/Voigtlander
const std::vector<interfaceDescription> heliarHartingVoigtlander = {
    interfaceDescription( 41.000f, 1.600f, 13.0f, 1.564f, 43.8f ),
    interfaceDescription( 25.760f, 3.600f, 12.0f, 1.609f, 58.9f ),
    interfaceDescription( -583.800f, 8.100f, 12.0f ),
    interfaceDescription( -44.760f, 1.600f, 9.5f, 1.564f, 43.8f ),
    interfaceDescription( 44.760f, 3.000f, 9.5f ),
    interfaceDescription( inf,       5.100f, 9.3f ),
    interfaceDescription( 583.800f, 3.600f, 13.0f, 1.609f, 58.9f ),
    interfaceDescription( -25.760f, 1.600f, 13.0f, 1.564f, 43.8f ),
    interfaceDescription( -41.000f, 85.306f, 13.0f )
};

// Heliar DRP636166/1933 Deser/Voigtlander
const std::vector<interfaceDescription> heliarDeserVoigtlander = {
    interfaceDescription( 43.000f, 9.000f, 18.3f, 1.658f, 50.9f ),
    interfaceDescription( -85.000f, 3.000f, 18.3f, 1.581f, 40.9f ),
    interfaceDescription( inf,      7.000f, 18.3f ),
    interfaceDescription( -58.000f, 4.000f, 15.7f, 1.581f, 40.9f ),
    interfaceDescription( 34.500f, 5.000f, 15.7f ),
    interfaceDescription( inf,      4.000f, 14.1f ),
    interfaceDescription( 130.000f, 10.000f, 15.2f, 1.658f, 50.9f ),
    interfaceDescription( -20.500f, 3.000f, 15.2f, 1.607f, 40.3f ),
    interfaceDescription( -57.500f, 78.635f, 15.2f )
};

// Tessar USP2995980 / Zimmerman
const std::vector<interfaceDescription> tessarZimmerman = {
    interfaceDescription( 44.650f, 6.700f, 19.0f, 1.691f, 54.7f ),
    interfaceDescription( -267.950f, 4.000f, 19.0f ),
    interfaceDescription( inf,       3.000f, 16.7f ),
    interfaceDescription( -49.040f, 5.400f, 16.8f, 1.755f, 27.6f ),
    interfaceDescription( -26.710f, 3.000f, 16.8f, 1.640f, 34.6f ),
    interfaceDescription( 34.870f, 4.800f, 16.8f ),
    interfaceDescription( -1326.300f, 3.000f, 16.5f, 1.626f, 35.7f ),
    interfaceDescription( 29.160f, 9.270f, 16.5f, 1.744f, 44.8f ),
    interfaceDescription( -49.930f, 84.745f, 16.5f )
};

// Petzval Portrait Lens F/3.3 HFOV = 17deg
const std::vector<interfaceDescription> petzvalPortraitF33 = {
    interfaceDescription( 55.900f, 4.700f, 15.1f, 1.518f, 59.0f ),
    interfaceDescription( -43.700f, 0.800f, 15.1f, 1.575f, 41.5f ),
    interfaceDescription( 460.400f, 16.800f, 15.1f ),
    interfaceDescription( inf,      16.800f, 13.1f ),
    interfaceDescription( 110.600f, 1.500f, 15.0f, 1.575f, 41.5f ),
    interfaceDescription( 38.900f, 3.300f, 15.0f ),
    interfaceDescription( 48.000f, 3.600f, 15.0f, 1.517f, 64.2f ),
    interfaceDescription( -157.800f, 70.731f, 15.0f )
};

// Rolf Muller USP 4647161, 16mm F/4 155.9deg Fish Eye Lens #1
const std::vector<interfaceDescription> rolfMullerFishEyeF4 = {
    interfaceDescription( 302.249f, 8.335f, 151.7f, 1.620f, 60.3f ),
    interfaceDescription( 113.931f, 74.136f, 103.4f ),
    interfaceDescription( 752.019f, 10.654f, 89.0f, 1.639f, 55.4f ),
    interfaceDescription( 83.349f, 111.549f, 67.1f ),
    interfaceDescription( 95.882f, 20.054f, 45.1f, 1.654f, 33.7f ),
    interfaceDescription( 438.677f, 53.895f, 40.7f ),
    interfaceDescription( inf,      14.163f, 30.4f ),
    interfaceDescription( 294.541f, 21.934f, 29.8f, 1.517f, 64.2f ),
    interfaceDescription( -52.265f, 9.714f, 29.2f, 1.805f, 25.4f ),
    interfaceDescription( -142.884f, 0.627f, 29.8f ),
    interfaceDescription( -223.726f, 9.400f, 29.8f, 1.673f, 32.2f ),
    interfaceDescription( -150.404f, 231.683f, 32.6f )
};

// S. H. Brewer USP 3029699, F/1 144deg Wide Angle Lens
const std::vector<interfaceDescription> brewerWideAngleF1 = {
    interfaceDescription( 5016.060f, 172.753f, 1300.2f, 1.517f, 64.2f ),
    interfaceDescription( 735.020f, 691.012f, 704.3f ),
    interfaceDescription( -1685.396f, 138.176f, 692.2f, 1.620f, 60.3f ),
    interfaceDescription( 1021.270f, 27.622f, 627.3f ),
    interfaceDescription( 1112.896f, 561.397f, 629.3f, 1.648f, 33.8f ),
    interfaceDescription( -1070.093f, 138.176f, 611.3f, 1.620f, 60.3f ),
    interfaceDescription( 3212.285f, 3314.880f, 561.8f ),
    interfaceDescription( 2441.149f, 86.343f, 428.0f, 1.670f, 47.2f ),
    interfaceDescription( -4823.443f, 3.478f, 428.0f ),
    interfaceDescription( 635.368f, 86.343f, 387.9f, 1.673f, 32.2f ),
    interfaceDescription( 1052.704f, 3.478f, 381.2f ),
    interfaceDescription( 421.349f, 155.498f, 334.4f, 1.697f, 56.4f ),
    interfaceDescription( 1739.570f, 34.577f, 321.0f, 1.717f, 29.5f ),
    interfaceDescription( 273.208f, 131.621f, 220.7f ),
    interfaceDescription( inf,     80.257f, 212.8f ),
    interfaceDescription( -372.125f, 34.577f, 211.7f, 1.717f, 29.5f ),
    interfaceDescription( 846.711f, 179.642f, 244.1f, 1.620f, 60.3f ),
    interfaceDescription( -481.542f, 3.478f, 262.8f ),
    interfaceDescription( 735.020f, 117.443f, 273.5f, 1.697f, 56.4f ),
    interfaceDescription( -44343.975f, 3.478f, 266.2f ),
    interfaceDescription( 635.368f, 152.020f, 254.8f, 1.700f, 47.8f ),
    interfaceDescription( -1591.763f, 34.577f, 234.1f ),
    interfaceDescription( -1591.763f, 53.505f, 200.6f, 1.670f, 47.2f ),
    interfaceDescription( 5396.277f, 277.190f, 200.6f )
};

// F/4.5 25deg HFOV Dogmar, US 1,108,307
const std::vector<interfaceDescription> dogmarF45 = {
    interfaceDescription( 27.700f, 4.200f, 11.1f, 1.614f, 56.4f ),
    interfaceDescription( -103.100f, 1.800f, 11.1f ),
    interfaceDescription( -53.900f, 1.600f, 10.5f, 1.567f, 42.8f ),
    interfaceDescription( 37.700f, 2.700f, 9.9f ),
    interfaceDescription( inf,      2.700f, 9.8f ),
    interfaceDescription( -63.300f, 1.600f, 9.9f, 1.548f, 45.8f ),
    interfaceDescription( 35.100f, 1.800f, 10.5f ),
    interfaceDescription( 53.200f, 3.600f, 10.5f, 1.614f, 56.4f ),
    interfaceDescription( -35.700f, 88.556f, 10.5f )
};

// F/12.5 31deg Protar, US 895,045/08 Zeiss
const std::vector<interfaceDescription> protarF125 = {
    interfaceDescription( 17.500f, 2.900f, 4.5f, 1.648f, 33.8f ),
    interfaceDescription( 5.800f, 1.300f, 3.7f, 1.603f, 38.0f ),
    interfaceDescription( 18.600f, 1.500f, 3.7f ),
    interfaceDescription( inf,      1.500f, 3.6f ),
    interfaceDescription( -12.800f, 1.100f, 3.7f, 1.517f, 52.2f ),
    interfaceDescription( 18.600f, 1.800f, 4.5f, 1.615f, 51.2f ),
    interfaceDescription( -14.300f, 95.013f, 4.5f )
};

// F/6.3 50deg HFOV Metrogon, US 2,325,275
const std::vector<interfaceDescription> metrogonF63 = {
    interfaceDescription( 16.440f, 4.400f, 15.5f, 1.610f, 56.7f ),
    interfaceDescription( 19.720f, 0.800f, 14.6f ),
    interfaceDescription( 21.100f, 1.970f, 14.6f, 1.610f, 56.7f ),
    interfaceDescription( 25.100f, 0.950f, 14.6f ),
    interfaceDescription( 13.080f, 0.690f, 11.0f, 1.717f, 29.5f ),
    interfaceDescription( 10.870f, 8.070f, 9.8f ),
    interfaceDescription( inf,      8.070f, 5.9f ),
    interfaceDescription( -10.870f, 0.650f, 9.8f, 1.717f, 29.5f ),
    interfaceDescription( -13.140f, 1.510f, 11.4f ),
    interfaceDescription( -26.340f, 7.870f, 17.6f, 1.610f, 56.7f ),
    interfaceDescription( -18.040f, 79.729f, 17.6f )
};

// J. Eggert et al., USP 3388956, F/4 15deg Telephoto #1
const std::vector<interfaceDescription> eggertTelephotoF4 = {
    interfaceDescription( 42.156f, 6.676f, 18.4f, 1.642f, 58.0f ),
    interfaceDescription( -88.214f, 1.945f, 18.2f, 1.728f, 28.4f ),
    interfaceDescription( -1800.073f, 1.183f, 17.4f ),
    interfaceDescription( -137.288f, 1.945f, 17.3f, 1.654f, 33.7f ),
    interfaceDescription( -530.649f, 40.290f, 16.8f ),
    interfaceDescription( inf,      11.913f, 5.4f ),
    interfaceDescription( 34.202f, 2.234f, 7.3f, 1.757f, 31.7f ),
    interfaceDescription( 417.496f, 3.859f, 7.3f ),
    interfaceDescription( -31.069f, 0.526f, 7.3f, 1.650f, 39.1f ),
    interfaceDescription( 69.782f, 20.320f, 7.5f )
};

// Merte 80deg HFOV F/7.7, USP 2,126,126
const std::vector<interfaceDescription> merteF77 = {
    interfaceDescription( inf,       6.550f, 111.0f, 1.623f, 56.9f ),
    interfaceDescription( 215.560f, 101.280f, 91.0f ),
    interfaceDescription( inf,       2.360f, 8.4f ),
    interfaceDescription( -42.700f, 6.810f, 10.0f, 1.518f, 59.0f ),
    interfaceDescription( -21.200f, 1.310f, 11.0f ),
    interfaceDescription( -16.150f, 2.990f, 11.0f, 1.551f, 49.6f ),
    interfaceDescription( 100.880f, 6.420f, 13.0f, 1.603f, 60.6f ),
    interfaceDescription( -23.080f, 158.279f, 13.0f )
};

// Robert Richter, USP 2031792, 66mm F/6.3 100deg
const std::vector<interfaceDescription> richterF63 = {
    interfaceDescription( 16.875f, 6.660f, 16.3f, 1.620f, 60.3f ),
    interfaceDescription( 24.825f, 0.030f, 16.3f ),
    interfaceDescription( 13.641f, 0.750f, 12.4f, 1.717f, 29.5f ),
    interfaceDescription( 11.025f, 9.735f, 10.6f ),
    interfaceDescription( inf,      9.735f, 5.9f ),
    interfaceDescription( -11.025f, 0.750f, 10.6f, 1.717f, 29.5f ),
    interfaceDescription( -13.641f, 0.030f, 12.4f ),
    interfaceDescription( -24.825f, 6.660f, 16.3f, 1.620f, 60.3f ),
    interfaceDescription( -16.875f, 77.809f, 16.3f )
};

// Yasunori Arai, USP 4447137, F/1.8 Telephoto #2
const std::vector<interfaceDescription> araiTelephotoF18 = {
    interfaceDescription( 52.892f, 9.556f, 28.9f, 1.620f, 60.3f ),
    interfaceDescription( 2978.682f, 0.074f, 28.9f ),
    interfaceDescription( 38.766f, 3.571f, 25.6f, 1.723f, 37.9f ),
    interfaceDescription( 23.194f, 11.112f, 20.7f, 1.697f, 55.5f ),
    interfaceDescription( 69.755f, 3.000f, 19.7f ),
    interfaceDescription( 341.551f, 6.052f, 19.6f, 1.673f, 32.1f ),
    interfaceDescription( 22.422f, 18.520f, 14.5f ),
    interfaceDescription( inf,      6.949f, 12.4f ),
    interfaceDescription( -30.641f, 3.237f, 12.5f, 1.702f, 41.2f ),
    interfaceDescription( -29.652f, 5.089f, 13.3f ),
    interfaceDescription( 53.327f, 3.000f, 15.4f, 1.720f, 46.0f ),
    interfaceDescription( 106.470f, 2.963f, 15.4f ),
    interfaceDescription( 184.198f, 2.963f, 15.6f, 1.700f, 48.1f ),
    interfaceDescription( 1116.126f, 30.191f, 15.6f )
};

// T. Tsunashima, USP 4163603, F/2 84deg Wide Angle
const std::vector<interfaceDescription> tsunashimaF2WideAngle = {
    interfaceDescription( 346.939f, 16.610f, 88.0f, 1.670f, 57.3f ),
    interfaceDescription( 1632.653f, 0.780f, 83.0f ),
    interfaceDescription( 193.878f, 15.840f, 69.0f, 1.773f, 49.6f ),
    interfaceDescription( 60.943f, 13.270f, 48.0f ),
    interfaceDescription( 120.408f, 5.140f, 48.0f, 1.697f, 55.5f ),
    interfaceDescription( 60.310f, 14.650f, 42.0f ),
    interfaceDescription( 239.559f, 27.710f, 40.0f, 1.668f, 41.9f ),
    interfaceDescription( -2487.845f, 0.410f, 44.0f ),
    interfaceDescription( 152.959f, 4.370f, 44.0f, 1.697f, 55.5f ),
    interfaceDescription( 91.073f, 8.490f, 44.0f ),
    interfaceDescription( 121.633f, 29.390f, 44.2f, 1.749f, 35.0f ),
    interfaceDescription( -906.759f, 5.000f, 45.3f ),
    interfaceDescription( inf,       5.490f, 43.6f ),
    interfaceDescription( -1791.461f, 15.840f, 48.9f, 1.564f, 60.8f ),
    interfaceDescription( -90.808f, 18.610f, 47.0f ),
    interfaceDescription( -112.057f, 9.510f, 45.7f, 1.743f, 49.3f ),
    interfaceDescription( -91.073f, 3.180f, 42.5f, 1.805f, 25.5f ),
    interfaceDescription( 220.816f, 5.550f, 42.5f ),
    interfaceDescription( -676.045f, 13.470f, 46.0f, 1.743f, 49.3f ),
    interfaceDescription( -87.347f, 0.410f, 46.0f ),
    interfaceDescription( 808.163f, 14.650f, 55.0f, 1.697f, 55.5f ),
    interfaceDescription( -190.857f, 157.794f, 55.0f )
};

// Kikuo Momiyam, USP 4437735, F/2 94deg Wide Angle #1
const std::vector<interfaceDescription> momiyamaF2WideAngle = {
    interfaceDescription( 238.520f, 24.420f, 120.0f, 1.589f, 61.3f ),
    interfaceDescription( 566.770f, 0.730f, 120.0f ),
    interfaceDescription( 150.610f, 7.810f, 90.0f, 1.786f, 44.2f ),
    interfaceDescription( 68.820f, 27.350f, 62.5f ),
    interfaceDescription( 174.880f, 5.860f, 62.5f, 1.806f, 40.9f ),
    interfaceDescription( 70.340f, 16.600f, 55.0f ),
    interfaceDescription( 177.050f, 16.110f, 55.0f, 1.805f, 25.5f ),
    interfaceDescription( -1764.080f, 6.350f, 50.0f ),
    interfaceDescription( 240.220f, 4.880f, 45.0f, 1.713f, 53.8f ),
    interfaceDescription( 79.250f, 20.360f, 45.0f ),
    interfaceDescription( 178.370f, 17.090f, 44.2f, 1.755f, 27.5f ),
    interfaceDescription( -303.550f, 36.530f, 45.3f, 1.516f, 64.1f ),
    interfaceDescription( -86.890f, 4.150f, 48.9f ),
    interfaceDescription( 1014.000f, 41.020f, 45.7f, 1.773f, 49.6f ),
    interfaceDescription( -69.820f, 4.880f, 43.3f, 1.755f, 27.5f ),
    interfaceDescription( 230.520f, 10.650f, 42.5f ),
    interfaceDescription( -134.410f, 4.880f, 47.0f, 1.883f, 40.8f ),
    interfaceDescription( 159.190f, 29.540f, 56.0f, 1.487f, 70.2f ),
    interfaceDescription( -107.770f, 0.730f, 58.0f ),
    interfaceDescription( 555.270f, 21.490f, 75.0f, 1.697f, 55.5f ),
    interfaceDescription( -171.520f, 0.730f, 75.0f ),
    interfaceDescription( -5794.060f, 20.020f, 86.0f, 1.773f, 49.6f ),
    interfaceDescription( -221.470f, 178.126f, 86.0f )
};

// Roossinov 56deg HFOV F/8.3, USP 2,516,724
const std::vector<interfaceDescription> roossinovF83 = {
    interfaceDescription( 50.610f, 3.080f, 43.8f, 1.639f, 45.2f ),
    interfaceDescription( 32.470f, 50.920f, 32.2f ),
    interfaceDescription( 68.060f, 13.160f, 18.0f, 1.613f, 58.6f ),
    interfaceDescription( -35.120f, 4.390f, 18.0f, 1.548f, 45.8f ),
    interfaceDescription( 188.790f, 9.350f, 9.0f ),
    interfaceDescription( inf,       0.350f, 7.5f ),
    interfaceDescription( -185.390f, 4.320f, 9.0f, 1.548f, 45.8f ),
    interfaceDescription( 34.490f, 12.940f, 18.0f, 1.613f, 58.6f ),
    interfaceDescription( -66.830f, 50.000f, 18.0f ),
    interfaceDescription( -31.870f, 3.020f, 31.6f, 1.626f, 35.7f ),
    interfaceDescription( -49.440f, 52.779f, 42.0f )
};

// Krietzer, USP 4,333,714
const std::vector<interfaceDescription> krietzer = {
    interfaceDescription( 608.639f, 9.176f, 39.3f, 1.564f, 60.8f ),
    interfaceDescription( -903.196f, 0.714f, 39.3f ),
    interfaceDescription( 165.547f, 7.141f, 36.8f, 1.697f, 55.5f ),
    interfaceDescription( 41.802f, 48.093f, 30.4f ),
    interfaceDescription( 96.493f, 14.208f, 28.6f, 1.762f, 40.3f ),
    interfaceDescription( -447.860f, 12.960f, 28.6f ),
    interfaceDescription( -192.690f, 12.496f, 27.2f, 1.617f, 53.9f ),
    interfaceDescription( -74.976f, 44.995f, 27.5f ),
    interfaceDescription( -55.953f, 4.998f, 23.9f, 1.785f, 26.1f ),
    interfaceDescription( 202.827f, 2.821f, 25.7f ),
    interfaceDescription( -305.158f, 9.390f, 26.8f, 1.720f, 50.3f ),
    interfaceDescription( -63.080f, 0.714f, 26.8f ),
    interfaceDescription( -3365.077f, 8.605f, 28.6f, 1.697f, 55.5f ),
    interfaceDescription( -104.488f, 138.642f, 28.6f )
};

class lensSystem {
public:
	lensSystem() {};
	lensSystem( const std::vector< interfaceDescription > &elementsSrc_in );

	void clear();
	void loadSystem( const std::vector< interfaceDescription > &elementsSrc_in );

	// storage is used to create the buffers:
		// outline segments
	std::vector<glm::vec3> lensOutlines;

	// other data caching tbd
	std::vector< interfaceDescription > elementsSrc;

	// other lens parameters in the buffer
	GPULensDescription lensParameters;
};


// some common film sizes (units of mm)
static const char*  frameNames[] = {
	"iPhone 6", "iPhone XS", "Super 8", "Super 16", "Micro Four Thirds",
	"Super 35", "Academy 35", "APS-C (Fuji)", "Full Frame",
	"Hasselblad H6D-50C", "120 Film (6x7)", "Dynavision", "Todd-AO",
	"Phase One IQ4", "IMAX 15/70"
};
static const glm::vec2 frameSizes[] = {
	glm::vec2(4.8f, 3.6f), glm::vec2(5.76f, 4.29f), glm::vec2(5.79f, 4.01f), glm::vec2(12.5f, 7.41f), glm::vec2(17.3f, 13.0f),
	glm::vec2(24.89f, 18.66f), glm::vec2(21.95f, 16.00f), glm::vec2(23.6f, 15.60f), glm::vec2(36.00f, 24.00f),
	glm::vec2(43.8f, 32.9f), glm::vec2(56.00f, 67.00f), glm::vec2(52.63f, 37.72f), glm::vec2(52.63f, 23.01f),
	glm::vec2(54.0f, 40.0f), glm::vec2(70.41f, 52.63f)
};

class Camera {
  public:
	glm::vec3 position;
	glm::vec3 front;
	glm::vec3 up;

	float yaw;
	float pitch;
	float fov;

	float movementSpeed;
	float mouseSensitivity;

    Camera(glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f))
        : position(position), front(glm::vec3(0.0f, 0.0f, -1.0f)), up(glm::vec3(0.0f, 1.0f, 0.0f)),
          yaw(-90.0f), pitch(0.0f), fov(45.0f), movementSpeed(2.5f), mouseSensitivity(0.1f) {}

    glm::mat4 getViewMatrix() { return glm::lookAt(position, position + front, up); }

    void processKeyboard(int direction, float deltaTime) {
		float velocity = movementSpeed * deltaTime;
		if (direction == 0)
			position += front * velocity;
		if (direction == 1)
			position -= front * velocity;
		if (direction == 2)
			position -= glm::normalize(glm::cross(front, up)) * velocity;
		if (direction == 3)
			position += glm::normalize(glm::cross(front, up)) * velocity;
    	if (direction == 4)
    		up = glm::vec3( ( glm::rotate( glm::mat4(1.0f), velocity, front ) * vec4( up, 0.0f ) ) );
    	if (direction == 5)
    		up = glm::vec3( ( glm::rotate( glm::mat4(1.0f), -velocity, front ) * vec4( up, 0.0f ) ) );
    }

    void processMouseMovement(float xOffset, float yOffset) {
        xOffset *= mouseSensitivity;
        yOffset *= mouseSensitivity;

        yaw += xOffset;
        pitch += yOffset;

        if (pitch > 89.0f)
            pitch = 89.0f;
        if (pitch < -89.0f)
            pitch = -89.0f;

        glm::vec3 direction;
        direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
        direction.y = sin(glm::radians(pitch));
        direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
        front = glm::normalize(direction);
    }

	void processMouseScroll(float yOffset) {
		fov -= yOffset;
		if ( fov < 0.1f )
			fov = 0.1f;
		if ( fov > 180.0f )
			fov = 180.0f;
	}
};

struct SceneOrbit {
	glm::vec3 center = glm::vec3( 0.0f );
	glm::mat4 rotation = glm::mat4( 1.0f );
	float mouseSensitivity = 0.006f;

	void processMouseDrag( float xOffset, float yOffset, const Camera& camera ) {
		glm::mat4 yaw =
			glm::rotate( glm::mat4( 1.0f ), xOffset * mouseSensitivity, glm::vec3( 0.0f, 1.0f, 0.0f ) );
		glm::vec3 right = glm::normalize( glm::cross( camera.front, camera.up ) );
		glm::mat4 pitch = glm::rotate( glm::mat4( 1.0f ), yOffset * mouseSensitivity, right );
		rotation = yaw * pitch * rotation;
	}

	void roll ( float amount, const Camera& camera ) {
		rotation = glm::rotate( glm::mat4( 1.0f ), amount, camera.front ) * rotation;
	}

	glm::mat4 modelMatrix() const {
		return glm::translate( glm::mat4( 1.0f ), center ) * rotation *
			   glm::translate( glm::mat4( 1.0f ), -center );
	}

	glm::vec3 cameraPositionInScene( const glm::vec3& cameraPosition ) const {
		return glm::vec3( glm::inverse( modelMatrix() ) * glm::vec4( cameraPosition, 1.0f ) );
	}
};

#endif
