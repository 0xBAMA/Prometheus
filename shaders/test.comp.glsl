#version 460
//=============================================================================================================================
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
//=============================================================================================================================
layout ( local_size_x = 16, local_size_y = 16 ) in;
//=============================================================================================================================
#include "common.h"
#include "random.h"
#include "draine2.h" // phase function
#include "pbrConstants.glsl"
#include "hg_sdf.h"
#include "wood.h"
//=============================================================================================================================
layout ( rgba32f, set = 0, binding = 1 ) uniform image2D image; // accumulator image
//=============================================================================================================================
struct ray_t {
	vec3 origin;
	vec3 direction;
};
//=============================================================================================================================
float hash(vec3 p)  // replace this by something better
{
	p  = fract( p*0.3183099+.1 );
	p *= 17.0;
	return fract( p.x*p.y*p.z*(p.x+p.y+p.z) );
}

float noise( in vec3 x )
{
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f*f*(3.0-2.0*f);

	return mix(mix(mix( hash(i+vec3(0,0,0)),
	hash(i+vec3(1,0,0)),f.x),
	mix( hash(i+vec3(0,1,0)),
	hash(i+vec3(1,1,0)),f.x),f.y),
	mix(mix( hash(i+vec3(0,0,1)),
	hash(i+vec3(1,0,1)),f.x),
	mix( hash(i+vec3(0,1,1)),
	hash(i+vec3(1,1,1)),f.x),f.y),f.z);
}

float noiseFBM( in vec3 pos ) {
	const mat3 m = mat3(
	0.00,  0.80,  0.60,
	-0.80,  0.36, -0.48,
	-0.60, -0.48,  0.64
	);
	vec3 q = 8.0*pos;
	float f = 0.5000*noise( q ); q = m*q*2.01;
	f += 0.2500*noise( q ); q = m*q*2.02;
	f += 0.1250*noise( q ); q = m*q*2.03;
	f += 0.0625*noise( q ); q = m*q*2.01;
	return f;
}
//=============================================================================================================================
int bounce = 0;
//=============================================================================================================================
float dBBox( vec3 p ) {
	return fBox( p, vec3( 100.0f ) );
}
//=============================================================================================================================
#define fold45(p)(p.y>p.x)?p.yx:p
float deTemple(vec3 p) {
	p.z *= -1.0f;
	float scale = 2.1, off0 = .8, off1 = .3, off2 = .83;
	vec3 off =vec3(2.,.2,.1);
	float s=1.0;
	for(int i = 0;++i<20;) {
		p.xy = abs(p.xy);
		p.xy = fold45(p.xy);
		p.y -= off0;
		p.y = -abs(p.y);
		p.y += off0;
		p.x += off1;
		p.xz = fold45(p.xz);
		p.x -= off2;
		p.xz = fold45(p.xz);
		p.x += off1;
		p -= off;
		p *= scale;
		p += off;
		s *= scale;
	}
	return length(p)/s;
}

float deCage ( vec3 p ) {
	float d = 0.0f;
	float t = 0.3f;
	float s = 1.0f;
	vec4 r = vec4( 0.0f );
	vec4 q = vec4( p, 0.0f );
	for ( int j = 0; j < 4 ; j++ )
	r = max( r *= r *= r = mod( q * s + 1.0f, 2.0f ) - 1.0f, r.yzxw ),
	d = max( d, ( 0.27f - length( r ) * 0.3f ) / s ),
	s *= 3.1f;
	return d;
}

float deCaves( vec3 p ){
	float scalar = 15.0f;
	float dbox = dBBox( p );
	if ( dbox > 0.0f ) {
		return dbox;
	}

	p.z -= 10.0f;
	p /= scalar;
	vec3 Q, U = vec3( 1.0f );
	float d=1.0, a=1.0f;
	d = min( length( fract( p.xz) -0.5f ) - 0.2f, 0.3f - abs( p.y - 0.2f ) );
	for( int j = 0; j++ < 9; a += a )
	Q = p * a * 9.0f,
	Q.yz *= Rotate2D( a ),
	d += abs( dot( sin( Q ), U ) ) / a * 0.02f;
	return d * 0.6f * scalar + 0.6f * noiseFBM( p * 6.0f ) * noise( p * 2.0f );
}

float deLumpy(vec3 p){
	vec3 pOrig = p;

	float dbox = dBBox( p );
	if ( dbox > 0.0f ) {
		return dbox;
	}

	float scalar = 12.0f;
	p /= scalar;

	vec3 Q;
	float i,j,d=1.,a;
	d=dot(sin(p),cos(p.yzx))+1.2;
	a=1.;
	for(j=0.;j++<14.;)
	Q=(p+fract(sin(j)*3e3)*9.)*a,
	Q+=sin(Q*1.05)*2.,
	Q=sin(Q),
	d+=Q.x*Q.y*Q.z/a*.4,
	a*=2.;
	return d*.4 * scalar - 4.6f * noiseFBM( p * 6.0f ) * ( pow( noise( p * 2.0f ), 2.0f ) - 0.2f );
}

float deChunky(vec3 p){
	float dbox = dBBox( p );
	if ( dbox > 0.0f ) {
		return dbox;
	}

	float scalar = 12.0f;
	p/=scalar;
	vec3 Q;
	float i,j,d=1.,a;
	d=min(p.y,0.)+.3;
	a=1.;
	for(j=0.;j++<14.;)
	Q=(p+vec3(9,0,0)+fract(sin(j)*1e3)*6.283)*a,
	Q+=sin(Q)*2.,
	Q=sin(Q),
	d+=Q.x*Q.y*Q.z/a,
	a*=2.;
	return ( d*.3 * scalar - 6.6f * noiseFBM( p * 6.0f ) * pow( noise( p * 1.0f ), 7.0f ) );
}

float deSmooth(vec3 p){
	float d = 1e5;
	const int n = 3;
	const float fn = float(n);
	for(int i = 0; i < n; i++){
		vec3 q = p;
		float a = float(i)*fn*2.422; //*6.283/fn
		a *= a;
		q.z += float(i)*float(i)*1.67; //*3./fn
		q.xy *= Rotate2D(a);
		float b = (length(length(sin(q.xy) + cos(q.yz))) - .15);
		float f = max(0., 1. - abs(b - d));
		d = min(d, b) - .25*f*f;
	}
	return d;
}

vec3 Rotate(vec3 z,float AngPFXY,float AngPFYZ,float AngPFXZ) {
	float sPFXY = sin(radians(AngPFXY)); float cPFXY = cos(radians(AngPFXY));
	float sPFYZ = sin(radians(AngPFYZ)); float cPFYZ = cos(radians(AngPFYZ));
	float sPFXZ = sin(radians(AngPFXZ)); float cPFXZ = cos(radians(AngPFXZ));

	float zx = z.x; float zy = z.y; float zz = z.z; float t;

	// rotate BACK
	t = zx; // XY
	zx = cPFXY * t - sPFXY * zy; zy = sPFXY * t + cPFXY * zy;
	t = zx; // XZ
	zx = cPFXZ * t + sPFXZ * zz; zz = -sPFXZ * t + cPFXZ * zz;
	t = zy; // YZ
	zy = cPFYZ * t - sPFYZ * zz; zz = sPFYZ * t + cPFYZ * zz;
	return vec3(zx,zy,zz);
}

vec4 OrbitTrap;
float deIFS( vec3 p ) {
	float Scale = 1.34f;
	float FoldY = 1.025709f;
	float FoldX = 1.025709f;
	float FoldZ = 0.035271f;
	float JuliaX = -1.763517f;
	float JuliaY = 0.392486f;
	float JuliaZ = -1.734913f;
	float AngX = -51.080209f;
	float AngY = 0.0f;
	float AngZ = -29.096322f;
	float Offset = -3.036726f;
	bool EnableOffset = true;
	int Iterations = 80;
	float Precision = 1.0f;
	// output _sdf c = _SDFDEF)

	OrbitTrap = vec4(0.0f);
	float u2 = 1;
	float v2 = 1;
	if(EnableOffset)p = Offset+abs(vec3(p.x,p.y,p.z));

	vec3 p0 = vec3(JuliaX,JuliaY,JuliaZ);
	float l = 0.0;
	int i=0;
	for (i=0; i<Iterations; i++) {
		p = Rotate(p,AngX,AngY,AngZ);
		p.x=abs(p.x+FoldX)-FoldX;
		p.y=abs(p.y+FoldY)-FoldY;
		p.z=abs(p.z+FoldZ)-FoldZ;
		p=p*Scale+p0;
		l=length(p);
		float rr = dot(p,p);

		OrbitTrap.r = max( OrbitTrap.r, rr );
		// hitColor = vec3( abs( p / 10.0f ) );
	}
	return Precision*(l)*pow(Scale, -float(i));
}

mat3 rotZ ( float t ) {
	float s = sin( t );
	float c = cos( t );
	return mat3( c, s, 0., -s, c, 0., 0., 0., 1. );
}

mat3 rotX ( float t ) {
	float s = sin( t );
	float c = cos( t );
	return mat3( 1., 0., 0., 0., c, s, 0., -s, c );
}

mat3 rotY ( float t ) {
	float s = sin( t );
	float c = cos( t );
	return mat3 (c, 0., -s, 0., 1., 0, s, 0, c);
}

float deIFS2 ( vec3 p ){
	vec2 rm = radians( 360.0 ) * vec2( 0.468359, 0.95317 ); // vary x,y 0.0 - 1.0
	mat3 scene_mtx = rotX( rm.x ) * rotY( rm.x ) * rotZ( rm.x ) * rotX( rm.y );
	float scaleAccum = 1.;
	for( int i = 0; i < 18; ++i ) {
		p.yz = sqrt( p.yz * p.yz + 0.16406 );
		p *= 1.21;
		scaleAccum *= 1.21;
		p -= vec3( 2.43307, 5.28488, 0.9685 );
		p = scene_mtx * p;
	}
	return length( p ) / scaleAccum - 0.15;
}
//=============================================================================================================================
struct intersection_t {
	float dTravel;
	vec3 albedo;
	vec3 normal;
	bool frontfaceHit;
	int materialID;
	float IoR;
	float roughness;

// tbd:
// material properties... IoR, roughness...
// absorption state...
};
//=============================================================================================================================
// default "constructor"
//=============================================================================================================================
intersection_t DefaultIntersection() {
	intersection_t intersection;
	intersection.dTravel = GlobalData.raymarchMaxDistance;
	intersection.albedo = vec3( 0.0f );
	intersection.normal = vec3( 0.0f );
	intersection.frontfaceHit = false;
	intersection.materialID = 0; // indicates an escaped ray
	intersection.IoR = 1.0f / 1.5f;
	intersection.roughness = 0.0f;
	return intersection;
}
//=============================================================================================================================
#define NOHIT						0
#define EMISSIVE					1
#define DIFFUSE						3
#define METALLIC					4
#define MIRROR						5
#define VOLUME_DENSE				6
#define VOLUME_SPARSE				7
//=============================================================================================================================
vec3 hitColor;
int hitSurfaceType;
float hitRoughness;

#define rot(a) mat2(cos(a),sin(a),-sin(a),cos(a))
float de( vec3 p ){
	const vec3 pOriginal = p;
	float sceneDist = 1000.0f;
	hitColor = vec3( 0.0f );
	hitSurfaceType = NOHIT;
	hitRoughness = 0.0f;

	const float dBounds = dBBox( p );

//	{
//		float d = distance( pOriginal, vec3( 0.0f ) ) - 2.0f;
//		if ( d < 0.5f ) {
//			const vec3 displacement = matWood( p * 0.8f );
//			d += 0.5f * displacement.r;
//		}
//		sceneDist = min( d, sceneDist );
//		if ( sceneDist == d && d < GlobalData.epsilon ) {
//			hitSurfaceType = ( rFloat() < 0.9f ) ? DIFFUSE : MIRROR;
//			hitColor = vec3( 0.99f );
//		}
//	}

	{
		float scalar = 20.0f;
		float d = deTemple( p.zyx / scalar ) * scalar;
		sceneDist = min( max( d, dBounds ), sceneDist );
		if ( sceneDist == d && d < GlobalData.epsilon ) {
			// hitSurfaceType = ( rFloat() < 0.9f ) ? DIFFUSE : MIRROR;
			hitSurfaceType = DIFFUSE;
//			hitRoughness = 0.01f;
			// hitColor = ( hitSurfaceType == MIRROR ) ? vec3( 0.99f ) : iron;
			// hitColor = vec3( 0.99f );
			hitColor = gold;
//			mix( tire, gold, noiseFBM( 0.1f * p + vec3( noise( 0.1f * p + vec3( 15.0f, 0.4f, 2.3f ) ), noise( 0.1f * p ), noise( 0.1f * p + vec3( 3.2f, 15.4f, 0.3f ) ) ) ) );
		}
	}

//	{
//		pMod1( p.y, 5.0f );
//		float d = fBox( p - vec3( 0.0f, 0.0f, 5.0f ), vec3( 150.0f, 5.0f, 0.5f ) );
//		sceneDist = min( d, sceneDist );
//		if ( sceneDist == d && d < GlobalData.epsilon ) {
//			hitSurfaceType = EMISSIVE;
//			hitColor = vec3( 5.0f );
//		}
//	}
	return sceneDist;
}

float density( vec3 p ) {

//	return 10.0f * step( 0.0f, -de2( p ) );
//	return noise( p * 1.0f );

	float val = ( deLumpy( p ) );
	if ( val > GlobalData.epsilon )
		return 0.0f;
	return noise( p * 6.0f );
//	return 1.0f;
//	return GetLuma( matWood( p * 0.1f ) ).r;

//	return 40.0f * pow( saturate( 2.0f * noise( p * 3.0f ) - 0.4f ), 6.0f ) * pow( saturate( 2.0f * noise( p * 13.0f ) - 0.4f ), 6.0f ) * step( 0.0f, max( -( length( p ) - 3.0f ), p.y ) );
//	return 0.01f + 0.5f * step( 0.0f, p.z + 1.5f );

}
//=============================================================================================================================
vec3 SDFNormal( in vec3 position ) {
	vec2 e = vec2( GlobalData.epsilon, 0.0f );
	return normalize( vec3( de( position ) ) - vec3( de( position - e.xyy ), de( position - e.yxy ), de( position - e.yyx ) ) );
}
//=============================================================================================================================
intersection_t raymarch ( in ray_t ray ) {
	intersection_t result = DefaultIntersection();
	float dQuery = 0.0f;
	float dTotal = 0.0f;
	vec3 pQuery = ray.origin;
	for ( int steps = 0; steps < ( max( 1, GlobalData.raymarchMaxSteps * ( 1.0f / max( float( bounce ), 1.0f ) ) ) ); steps++ ) {
		pQuery = ray.origin + dTotal * ray.direction;
		dQuery = de( pQuery );
		dTotal += dQuery * GlobalData.raymarchUnderstep;
		if ( dTotal > GlobalData.raymarchMaxDistance || abs( dQuery ) < GlobalData.epsilon ) {
			break;
		}
	}

	// fill out the result struct...
	result.dTravel = ( dQuery < GlobalData.epsilon ) ? dTotal : ( GlobalData.raymarchMaxDistance + 10.0f );
	result.materialID = hitSurfaceType;
	result.normal = SDFNormal( ray.origin + dTotal * ray.direction );
	result.frontfaceHit = true; // tbd
	result.roughness = hitRoughness;
	result.albedo = hitColor;
	result.IoR = 1.0f; // also tbd

	return result;
}

intersection_t deltaTrack( ray_t ray ) {
	intersection_t result = DefaultIntersection();
	vec3 hitPos = ray.origin;
	float tTotal = 0.0f;
	float maxDensity = 300.0f;
	float sd = -1.0f;

	for ( int i = 0; i < 1000; i++ ) {
		float t = -log( rFloat() ) / maxDensity;

		t = max( sd, t );

		hitPos += t * ray.direction;
		tTotal += t;

		sd = deLumpy( hitPos ) * 0.9f; // understep

		// if you hit
		if ( sd <= GlobalData.epsilon ) {
			if ( density( hitPos ) > rFloat() || tTotal > GlobalData.raymarchMaxDistance ) {
				break;
			}
		}
	}

	// fill out the intersection struct
	result.dTravel = tTotal;
	// result.albedo = mix( nvidia / 2.0f, vec3( 0.999f ), 0.8f ); // tbd, color should probably come from the density function
	result.albedo = mix( mix( nvidia / 2.0f, nvidia, 1.0f - noiseFBM( ( ray.origin + tTotal * ray.direction ) * 2.0f ) ), vec3( 0.99f ), 0.8f );
	result.normal = vec3( 0.0f ); // not appropriate to consider here
	result.frontfaceHit = true;  // ditto
	result.materialID = VOLUME_DENSE;
	result.IoR = 1.0f; // not significant
	result.roughness = 0.0f; // not significant

	return result;
}

intersection_t deltaTrackSparse( ray_t ray ) {
	intersection_t result = DefaultIntersection();
	vec3 hitPos = ray.origin;
	float tTotal = 0.0f;
	float maxDensity = 0.15f;
	float sd = -1.0f;

	for ( int i = 0; i < 100; i++ ) {
		float t = -log( rFloat() ) / maxDensity;

		t = max( sd, t );

		hitPos += t * ray.direction;
		tTotal += t;

		sd = dBBox( hitPos ) * 0.9f; // understep

		if ( ( ( sd < 0.0f ) ? 0.1f : 0.0f ) > rFloat() || tTotal > GlobalData.raymarchMaxDistance ) {
			break;
		}
	}

	// fill out the intersection struct
	result.dTravel = tTotal;
	result.albedo = sapphire; // tbd, color should probably come from the density function
	result.normal = vec3( 0.0f ); // not appropriate to consider here
	result.frontfaceHit = true;  // ditto
	result.materialID = VOLUME_SPARSE;
	result.IoR = 1.0f; // not significant
	result.roughness = 0.0f; // not significant

	return result;
}
//=============================================================================================================================
// when the scene intersection returns, you need to know what kind of scattering event is happening
//=============================================================================================================================
intersection_t getSceneIntersection ( ray_t ray ) {
	// evaluate the scene intersection based on this ray
	// I would like to add TinyBVH here + look into the BLAS/TLAS structure there for instancing

	intersection_t SDFResult = raymarch( ray );
	intersection_t DeltaDense = deltaTrack( ray );
	intersection_t DeltaSparse = deltaTrackSparse( ray );

	intersection_t result = DefaultIntersection();
	float minDistance = min( SDFResult.dTravel, min( DeltaDense.dTravel, DeltaSparse.dTravel ) );

	if ( minDistance == SDFResult.dTravel ) {
		result = SDFResult;
	} else if ( minDistance == DeltaDense.dTravel ) {
		result = DeltaDense;
	} else if ( minDistance == DeltaSparse.dTravel ) {
		result = DeltaSparse;
	}

	return result;
}


//=============================================================================================================================
void main () {
//=============================================================================================================================
	// initializing the RNG
	const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
	seed = PushConstants.wangSeed + 8675309 * pixel.x + 42069 * pixel.y;

//=============================================================================================================================
	// initial imagespace position for camera + jitter
	vec2 uv = ( ( vec2( pixel ) + rFloatN2() ) / ( GlobalData.presentBufferResolution ) ) * 2.0f - vec2( 1.0f );

//=============================================================================================================================
	// spherical camera logic, this will be replaced
	const float aspectRatio = float( imageSize( image ).x ) / float( imageSize( image ).y );
//	uv *= 0.4f;
//	uv.y /= aspectRatio;
//	uv.x -= 0.05f;
//	uv = vec2( atan( uv.y, uv.x ) + 0.5f, ( length( uv ) + 0.5f ) * acos( -1.0f ) );
//	vec3 baseVec = normalize( vec3( cos( uv.y ) * cos( uv.x ), sin( uv.y ), cos( uv.y ) * sin( uv.x ) ) );
//	baseVec = Rotate3D( pi / 2.0f, vec3( 2.5f, 0.4f, 1.0f ) ) * baseVec; // this is to match the other camera

	ray_t ray;
//	ray.direction = normalize( -baseVec.x * GlobalData.basisX + baseVec.y * GlobalData.basisY + ( 1.0f / GlobalData.FoV ) * baseVec.z * GlobalData.basisZ );
	ray.direction = normalize( aspectRatio * uv.x * GlobalData.basisX + uv.y * GlobalData.basisY + ( 1.0f / GlobalData.FoV ) * GlobalData.basisZ );
	ray.origin = GlobalData.viewerPosition;

//	ray.origin = GlobalData.FoV * ( aspectRatio * uv.x * GlobalData.basisX + uv.y * GlobalData.basisY ) + GlobalData.viewerPosition;
//	ray.direction = -1.0f * ( aspectRatio * uv.x * GlobalData.basisX + uv.y * GlobalData.basisY ) + vec3( GlobalData.basisZ );

//=============================================================================================================================
	// begin the process of taking a new sample
	vec3 color = vec3( 0.0f );

	// placeholder, I want to get into the habit of doing things spectrally
	vec3 transmission = vec3( 1.0f );
	vec3 accumulatedRadiance = vec3( 0.0f );

//=============================================================================================================================
	// pathtracing logic, starting with the incoming camera ray
	for ( bounce = 0; bounce < GlobalData.bounces; bounce++ ) {
		// scene intersection
			// "conductor" scene intersection - this is a regular surface with a normal
			// "dielectric" scene intersection - this is s surface with a normal + some other info (glass stuff)
			// "participating media" scene intersection - this is going to scatter using a uniform phase function for now
				// this should happen based on the delta tracking raymarch, low density everywhere
			// "nohit" scene intersection - ray escaped the scene without encountering a surface or volume scattering event

		intersection_t sceneIntersection = getSceneIntersection( ray );

		// kill the ray, once possible contribution becomes very small
		if ( max( transmission.x, max( transmission.y, transmission.z ) ) < 0.001f ) break;

		// epsilon bump for surfaces... returning a 0 vector for the normal on the volume stuff makes this work for both cases
		ray.origin = ray.origin + ray.direction * sceneIntersection.dTravel + 3.0f * GlobalData.epsilon * sceneIntersection.normal;

		if ( sceneIntersection.materialID != EMISSIVE ) transmission *= sceneIntersection.albedo;

	// direct lighting contribution
		// generate a point on the light

		// checking occlusion...
		ray_t shadowRay;
		shadowRay.origin = ray.origin;
#define POINTLIGHT
#ifdef POINTLIGHT
		vec3 pLight = vec3( 1000.0f, 1.0f * CircleOffset() );
		shadowRay.direction = normalize( pLight - shadowRay.origin );
#else
		shadowRay.direction = vec3( 1.0f, 0.0f, 0.0f );
#endif
		intersection_t lightOcclusion = getSceneIntersection( shadowRay );
		if ( lightOcclusion.dTravel >= GlobalData.raymarchMaxDistance )
			accumulatedRadiance += transmission * vec3( 1.0f );

		switch ( sceneIntersection.materialID ) {
			// ESCAPE
			case NOHIT:
			// this ray has escaped the scene to the sky, so we take a sky sample + kill it
			// accumulatedRadiance += transmission * max( 3.0f * dot( ray.direction, vec3( 0.0f, 0.0f, 1.0f ) ), 0.0f );
			// accumulatedRadiance += transmission * 13.0f * step( 0.8f, dot( ray.direction, vec3( 0.0f, 0.0f, -1.0f ) ) );
			bounce = GlobalData.bounces;
			break;

			// SURFACES
			case EMISSIVE:
			accumulatedRadiance += transmission * sceneIntersection.albedo;
			ray.direction = cosWeightedRandomHemisphereDirection( sceneIntersection.normal );
			break;

			case DIFFUSE:
			ray.direction = cosWeightedRandomHemisphereDirection( sceneIntersection.normal );
			break;

			case METALLIC:
			ray.direction = normalize( ( 1.0f + GlobalData.epsilon ) * sceneIntersection.normal + mix( reflect( ray.direction, sceneIntersection.normal ), RandomUnitVector(), sceneIntersection.roughness ) );
			break;

			case MIRROR:
			ray.direction = reflect( ray.direction, sceneIntersection.normal );
			break;

			// VOLUMES
			case VOLUME_DENSE:
			ray.direction = sampleApproxMieDirection( ray.direction, 15, rFloat(), rFloat(), rFloat() );
			break;

			case VOLUME_SPARSE:
			ray.direction = sampleApproxMieDirection( ray.direction, 5, rFloat(), rFloat(), rFloat() );
			break;

			default:
			break;
		}

		// russian roulette termination - alternative scheme based on https://bsky.app/profile/maxtarpini.bsky.social/post/3msgzb2s7bk2s
		// float maxChannel = max( transmission.r, max( transmission.g, transmission.b ) );
		float maxChannel = dot( transmission, vec3( 0.2126f, 0.7152f, 0.0722f ) );
		if ( rFloat() > maxChannel ) break;
		transmission *= 1.0f / maxChannel; // compensation term
	}

	color = clamp( accumulatedRadiance, 0.0f, 100.0f );

//=============================================================================================================================
	// load the previous color, mix the new and old values based on the current sampleCount
	const vec4 previousColor = imageLoad( image, pixel );
	const float sampleCount = previousColor.a + 1.0f;
	const float mixFactor = 1.0f / sampleCount;
	const vec4 mixedColor = vec4( ( any( isnan( color.rgb ) ) ) ?
		vec3( 0.0f ) : mix( previousColor.rgb, color.rgb, mixFactor ), sampleCount );

//=============================================================================================================================
	// and store it back
	imageStore( image, pixel, ( GlobalData.reset != 0 ) ? vec4( color, 1.0f ) : mixedColor );
}