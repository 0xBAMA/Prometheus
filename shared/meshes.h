#include <vk_types.h>

struct Vertex {
	glm::vec3 position;
	float uv_x;
	glm::vec3 normal;
	float uv_y;
	glm::vec4 color;
};

extern uint32_t Cube_vtx_count;
extern Vertex Cube_vtx[];
extern uint32_t Cube_idx_count;
extern uint32_t Cube_idx[];
extern uint32_t Sphere_vtx_count;
extern Vertex Sphere_vtx[];
extern uint32_t Sphere_idx_count;
extern uint32_t Sphere_idx[];
extern uint32_t Suzanne_vtx_count;
extern Vertex Suzanne_vtx[];
extern uint32_t Suzanne_idx_count;
extern uint32_t Suzanne_idx[];