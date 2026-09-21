// rayState_t setup for Phoenix
//=============================================================================================================================
// total 64 bytes
struct rayState_t {
	vec4 data1; // .xyz is origin, .w is wavelength
	vec4 data2; // .xyz is direction, .w is energy
	vec4 data3; // .xyz is normal vector, .w is transmission
	vec4 data4; // .x is mat/IoR, .y is roughness/albedo, .z is distance, .w is packed pixel index
};

/*
New version:

// pathtrace state -> 4 floats
	wavelength
	energy
	transmission
	pixel index -> can pack 2x 16-bit values

// spatial arrangement -> 6 floats
	origin
	direction

// intersection result -> 6 floats
	distance
	material/IoR -> can pack these two values together
	roughness -> can use half precision
	albedo -> can use half precision
	normal vector
*/
//=============================================================================================================================
void SetRayOrigin 		( inout rayState_t rayState, vec3 origin )		{ rayState.data1.xyz = origin; }
vec3 GetRayOrigin		( rayState_t rayState )							{ return rayState.data1.xyz; }

void SetRayDirection	( inout rayState_t rayState, vec3 direction )	{ rayState.data2.xyz = direction; }
vec3 GetRayDirection	( rayState_t rayState )							{ return rayState.data2.xyz; }

void SetWavelength		( inout rayState_t rayState, float wavelength )	{ rayState.data1.w = direction; }
float GetWavelength		( rayState_t rayState )							{ return data1.w; }

void SetEnergyTotal 	( inout rayState_t rayState, float energy )		{ rayState.data2.w = energy; }
float GetEnergyTotal	( rayState_t rayState )							{ return rayState.data2.w; }
void AddEnergy			( inout rayState_t rayState, float energy )		{ SetEnergyTotal( rayState, GetEnergyTotal( rayState ) + energy ); }

void SetTransmission	( inout rayState_t rayState, float transmission ) { rayState.data3.w = transmission; }
float GetTransmission	( rayState_t rayState )							{ return rayState.data3.w; }

void SetNormal			( inout rayState_t rayState, vec3 normal )		{ rayState.data3.xyz = normal; }
vec3 GetNormal			( rayState_t rayState )							{ return rayState.data3.xyz; }

void SetPixelIndex		( inout rayState_t rayState, ivec2 pixelIndex )	{ rayState.data4.w = uintBitsToFloat( packHalf2x16( vec2( pixelIndex ) ) ); }
ivec2 GetPixelIndex		( rayState_t rayState )							{ return ivec2( unpackHalf2x16( floatBitsToUint( rayState.data4.w ) ); }

void SetDistance		( inout rayState_t rayState, float distanceV )	{ rayState.data4.z = distanceV; }
float GetDistance		( rayState_t rayState )							{ return rayState.data4.z; }

// need set/get for:
	// mat/IoR
	// roughness/albedo
//=============================================================================================================================
void StateReset ( inout rayState_t rayState ) {
	// write zeroes
	rayState.data1 = rayState.data2 = rayState.data3 = rayState.data4 = vec4( 0.0f );

	// need sane defaults...
	SetTransmission( rayState, 1.0f );
	SetHitDistance( rayState, 1e30f );
}