#ifndef CAMERA_H
#define CAMERA_H

#include "glm/ext/matrix_float4x4.hpp"
#include "glm/ext/vector_float3.hpp"
#include "glm/geometric.hpp"
#include "glm/gtc/matrix_transform.hpp"

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

	glm::mat4 modelMatrix() const {
		return glm::translate( glm::mat4( 1.0f ), center ) * rotation *
			   glm::translate( glm::mat4( 1.0f ), -center );
	}

	glm::vec3 cameraPositionInScene( const glm::vec3& cameraPosition ) const {
		return glm::vec3( glm::inverse( modelMatrix() ) * glm::vec4( cameraPosition, 1.0f ) );
	}
};

#endif
