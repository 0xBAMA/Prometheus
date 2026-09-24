//=============================================================================================================================
// rayState_t setup for Phoenix
//=============================================================================================================================
#define NOHIT						0
#define EMISSIVE					1
#define DIFFUSE						3
#define METALLIC					4
#define MIRROR						5
#define VOLUME_DENSE				6
#define VOLUME_SPARSE				7
// eventually glass etc
//=============================================================================================================================
// total 64 bytes, significantly more memory efficient than Icarus
struct rayState_t {
	vec4 data1; // .xyz is origin, .w is wavelength
	vec4 data2; // .xyz is direction, .w is energy
	vec4 data3; // .xyz is normal vector, .w is transmission
	vec4 data4; // .x is mat/IoR, .y is roughness/albedo, .z is distance, .w is packed pixel index
}; // not sure how I'm going to indicate that a ray is a light trace, maybe using a leftover sign bit?
//=============================================================================================================================
// -> if you use the sign bit on the IoR to indicate that this ray is a light trace, you can use the mat value for the light index
	// and I think that gets you what you need for the additive contribution of the light trace
//=============================================================================================================================
void SetRayOrigin 		( inout rayState_t rayState, vec3 origin )		{ rayState.data1.xyz = origin; }
vec3 GetRayOrigin		( rayState_t rayState )							{ return rayState.data1.xyz; }

void SetRayDirection	( inout rayState_t rayState, vec3 direction )	{ rayState.data2.xyz = direction; }
vec3 GetRayDirection	( rayState_t rayState )							{ return rayState.data2.xyz; }

void SetWavelength		( inout rayState_t rayState, float wavelength )	{ rayState.data1.w = wavelength; }
float GetWavelength		( rayState_t rayState )							{ return rayState.data1.w; }

void SetEnergyTotal 	( inout rayState_t rayState, float energy )		{ rayState.data2.w = energy; }
float GetEnergyTotal	( rayState_t rayState )							{ return rayState.data2.w; }
void AddEnergy			( inout rayState_t rayState, float energy )		{ SetEnergyTotal( rayState, GetEnergyTotal( rayState ) + energy ); }

void SetTransmission	( inout rayState_t rayState, float transmission ) { rayState.data3.w = transmission; }
float GetTransmission	( rayState_t rayState )							{ return rayState.data3.w; }
void Attenuate			( inout rayState_t rayState, float albedo )		{ float trans = GetTransmission( rayState ); SetTransmission( rayState, trans * albedo ); }
bool isDead				( rayState_t rayState )							{ return rayState.data3.w == 0.0f; }
void Kill				( rayState_t rayState )							{ rayState.data3.w = 0.0f; }

void SetNormal			( inout rayState_t rayState, vec3 normal )		{ rayState.data3.xyz = normal; }
vec3 GetNormal			( rayState_t rayState )							{ return rayState.data3.xyz; }

// encoding in the length of the direction vector now (safer than using normal) / reusing for light id
void SetBounce			( inout rayState_t rayState, int bounce )		{ vec3 dir = GetRayDirection( rayState ); SetRayDirection( rayState, bounce * normalize( dir ) ); }
int GetBounce			( rayState_t rayState )							{ return int( length( GetRayDirection( rayState ) ) ); }

void SetPixelIndex		( inout rayState_t rayState, ivec2 pixelIndex )	{ rayState.data4.w = uintBitsToFloat( packHalf2x16( vec2( pixelIndex ) ) ); }
ivec2 GetPixelIndex		( rayState_t rayState )							{ return ivec2( unpackHalf2x16( floatBitsToUint( rayState.data4.w ) ) ); }

void SetDistance		( inout rayState_t rayState, float distanceV )	{ rayState.data4.z = distanceV; }
float GetDistance		( rayState_t rayState )							{ return rayState.data4.z; }

// makes sense to handle these in pairs, as they are always used together
void SetMatIoR			( inout rayState_t rayState, vec2 matIoR )		{ rayState.data4.x = uintBitsToFloat( packHalf2x16( matIoR ) ); }
vec2 GetMatIoR			( rayState_t rayState )							{ return unpackHalf2x16( floatBitsToUint( rayState.data4.x ) ); }

void SetRoughnessAlbedo	( inout rayState_t rayState, vec2 roughnessAlbedo )	{ rayState.data4.y = uintBitsToFloat( packHalf2x16( roughnessAlbedo ) ); }
vec2 GetRoughnessAlbedo	( rayState_t rayState )							{ return unpackHalf2x16( floatBitsToUint( rayState.data4.y ) ); }
//=============================================================================================================================
void StateReset ( inout rayState_t rayState ) {
	// write zeroes
	rayState.data1 = rayState.data2 = rayState.data3 = rayState.data4 = vec4( 0.0f );

	// need sane defaults...
	SetTransmission( rayState, 1.0f );
	SetDistance( rayState, 1e30f );
}