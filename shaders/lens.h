// 8 floats per interface
struct GPUInterfaceDescription {
	float radius;
	float axisPos;
	float semiAperture;
	float cosTerm; // contains a value which helps compute the intersection on the GPU faster (no runtime trig)
	float materialFront; // contains two half-floats with Cauchy A and B parameters
	float materialBack; // contains two half-floats with Cauchy A and B parameters
	float information; // contains some packed control bits
	float pad; // padding out to 8x 4 bytes
};

const float lensMaxDistance = 1e30f;

// should probably provide a wrapper here like, intersectElement()
	// which should indicate the distance to the intersection or max distance otherwise