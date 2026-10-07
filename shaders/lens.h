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

struct GPULensDescription {
	float totalSystemThickness; // this also includes the focus offset
	float numElements;
	vec2 filmSize;
	float maxSemiAperture;

	// maxing out at 32 interfaces for now, this is an easy thing to bump later
	GPUInterfaceDescription elements[ 32 ];
};

// a spherical cap represents a single spherical element in a lens system
float sphereCapIntersect( vec3 sphereCenter, float sphereRadius, vec3 capCenter, float cosThresh, vec3 rO, vec3 rD, inout vec3 normal ) {
    // first evaluate typical ray-sphere intersection
    const vec3 omc = rO - sphereCenter;
    const float b = dot(rD, omc);
    const float c = dot(omc, omc) - sphereRadius * sphereRadius;
    const float bsqmc = b * b - c;

    // there is 0, 1, or 2 roots of this quadratic
    float tClosest = 1e9f; // large positive distance, overwritten
    vec3 closestNormal = vec3( 0.0f );
    if ( bsqmc >= 0.0f ) { // the roots correspond to t = -b +/- sqrt(bsqmc) -> bsqmc must be non-negative
      // bsqmc == 0.0f means 1 root, ray is tangent
      // bsqmc  > 0.0f means 2 roots, evaluate smallest positive
        float rbsqmc = sqrt(bsqmc);

        // note:
            // if the roots are equal, eval once
            // if not, need to evaluate mask term for both roots
        for ( int i = 0 + ((bsqmc==0.0f)?1:0); i < 2; i++ ) {
            float  t = (i == 0) ? (-b - rbsqmc) : (-b + rbsqmc);

            vec3 hit = rO + rD * t;
            vec3 dNorm = normalize(hit - sphereCenter); // possibly some room for optimziation here, to skip normalize()
            bool   inArc = (dot(dNorm, capCenter) >= cosThresh);

            if (inArc && t > 0.0f && t < tClosest)
            { // update closest distance
                tClosest = t;

                // close root is a positive, but we have to invert for the more distant root
                closestNormal = (i == 0) ? dNorm : -dNorm;
            }
        }
    } else { // 0 roots, ray misses
        tClosest = -1.0f;
    }

    normal = closestNormal;
    return tClosest;
}

// a plane is being used to represent plano- elements in a lens system
float rayPlaneIntersect( vec3 norm, vec3 p, vec3 rO, vec3 rD) {
    // need to invert the normal, if it's facing the other direction
    // shadingNormal = ...

    return -(dot(rO - p, norm)) / dot(rD, norm);
}

float evaluateCauchy ( float A, float B, float wavelength ) {
	float wavelengthMicrons = wavelength / 1000.0f;
	const float wms = wavelengthMicrons * wavelengthMicrons;
	return A + B / wms;
}