#pragma once

#ifndef CAMERA_H
#define CAMERA_H

#include "glm/ext/matrix_float4x4.hpp"
#include "glm/ext/vector_float3.hpp"
#include "glm/geometric.hpp"
#include "glm/gtc/matrix_transform.hpp"

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
	interfaceDescription(1376.167f, 20.017f, 84.1f, 1.785f, 26.1f),
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
    		up = ( glm::rotate( velocity, front ) * vec4( up, 0.0f ) ).xyz();
    	if (direction == 5)
    		up = ( glm::rotate( -velocity, front ) * vec4( up, 0.0f ) ).xyz();
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
