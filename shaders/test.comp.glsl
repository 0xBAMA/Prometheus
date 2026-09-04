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
//=============================================================================================================================
layout ( rgba32f, set = 0, binding = 1 ) uniform image2D image; // accumulator image
//=============================================================================================================================
struct ray_t {
	vec3 origin;
	vec3 direction;
};
//=============================================================================================================================
int bounce = 0;
//=============================================================================================================================
//float de( vec3 p ){
//	float s = 2.;
//	float e = 0.;
//	for(int j=0;++j<7;)
//	p.xz=abs(p.xz)-2.3,
//	p.z>p.x?p=p.zyx:p,
//	p.z=1.5-abs(p.z-1.3+sin(p.z)*.2),
//	p.y>p.x?p=p.yxz:p,
//	p.x=3.-abs(p.x-5.+sin(p.x*3.)*.2),
//	p.y>p.x?p=p.yxz:p,
//	p.y=.9-abs(p.y-.4),
//	e=12.*clamp(.3/min(dot(p,p),1.),.0,1.)+
//	2.*clamp(.1/min(dot(p,p),1.),.0,1.),
//	p=e*p-vec3(7,1,1),
//	s*=e;
//	return length(p)/s;
//}

//float de(vec3 p){
//	const float scale = 2.0f;
//
//	p /= scale;
//	float d, a;
//	d=a=1.;
//	for(int j=0;j++<16;)
//	p.xz=abs(p.xz)*Rotate2D(pi/4.),
//	d=min(d,max(length(p.zx)-.3,p.y-.4)/a),
//	p.yx*=Rotate2D(.5),
//	p.y-=3.,
//	p*=1.5,
//	a*=1.5;
//	return d * scale;
//}

#define NOHIT						0
#define EMISSIVE					1
#define DIFFUSE						3
#define METALLIC					4
#define MIRROR						5

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

	{
		float scalar = 3.0f;
		p /= scalar;
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
		d *= scalar;
		p = pOriginal;
		sceneDist = min( d, sceneDist );
		if ( sceneDist == d && d < GlobalData.epsilon ) {
			hitSurfaceType = MIRROR;
			hitColor = iron;
		}
	}

	{
		float scalar = 3.0f;
		vec3 k = vec3( 5.0, 2.0, 1.0 );
		p.y += 5.5;
		p /= scalar;
		for( int j = 0; ++j < 8; ) {
			p.xz = abs( p.xz );
			p.xz = p.z > p.x ? p.zx : p.xz;
			p.z = 0.9 - abs( p.z - 0.9 );
			p.xy = p.y > p.x ? p.yx : p.xy;
			p.x -= 2.3;
			p.xy = p.y > p.x ? p.yx : p.xy;
			p.y += 0.1;
			p = k + ( p - k ) * 3.2;
		}
		float d = scalar * ( length( p ) / 6e3 - 0.001 );
		p = pOriginal;

		sceneDist = min( d, sceneDist );
		if ( sceneDist == d && d < GlobalData.epsilon ) {
			hitSurfaceType = ( rFloat() < 0.9f ) ? DIFFUSE : MIRROR;
			hitRoughness = 0.1f;
			hitColor = ( hitSurfaceType == MIRROR ) ? vec3( 0.99f ) : carrot;
		}
	}

	{
		p -= vec3( 0.0f, 0.0f, 0.0f );
		float r = 18.; // radius of the circle
		float l = length(p.xz) - r;
		float dRing = length(vec2(p.y, l)) - 1.618f;

		sceneDist = min( dRing, sceneDist );
		if ( sceneDist == dRing && dRing < GlobalData.epsilon ) {
			hitSurfaceType = EMISSIVE;
			hitColor = vec3( 10.0f );
		}
	}
	return sceneDist;
}

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

float de2(vec3 p){
	vec3 pOrig = p;
	float scalar = 2.0f;
	p /= scalar;
//	p.y *= -1.0f;
//	p.y += 2.0f;
//	float d, a;
//	d=a=1.;
//	for(int j=0;j++<18;)
//	p.xz=abs(p.xz)*Rotate2D(pi/4.),
//	d=min(d,max(length(p.zx)-.3,p.y-.4)/a),
//	p.yx*=Rotate2D(.5),
//	p.y-=3.,
//	p*=1.5,
//	a*=1.5;
//	return d * scalar - 0.03f * noiseFBM( pOrig * 5.0f );


	vec3 Q;
	float i,j,d=1.,a;
	d=dot(sin(p),cos(p.yzx))+1.2;
	a=1.;
	for(j=0.;j++<11.;)
		Q=(p+fract(sin(j)*3e3)*9.)*a,
		Q+=sin(Q*1.05)*2.,
		Q=sin(Q),
		d+=Q.x*Q.y*Q.z/a*.4,
		a*=2.;
	return d*.4 * scalar;
}

float density( vec3 p ) {

//	return 10.0f * step( 0.0f, -de2( p ) );
//	return noise( p * 1.0f );

	float val = de2( p );
	if ( val > GlobalData.epsilon )
		return 0.0f;
//	return noiseFBM( p );
	return 1.0f;
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
float raymarch ( in ray_t ray ) {
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
	return dTotal;
}
//=============================================================================================================================
// when the scene intersection returns, you need to know what kind of scattering event is happening
//=============================================================================================================================
float getSceneIntersection ( ray_t ray ) {
	// evaluate the scene intersection based on this ray
		// I would like to add TinyBVH here + look into the BLAS/TLAS structure there for instancing

	// I also want to consider the delta tracking raymarch

	// as a placeholder, I'm doing SDF intersection
	return raymarch( ray );
//	return 1000.0f;
}

float deltaTrack( ray_t ray ) {
	vec3 hitPos = ray.origin;
	float tTotal = 0.0f;
	float maxDensity = 50.0f;
	float sd = -1.0f;

	for ( int i = 0; i < 1000; i++ ) {
		float t = -log( rFloat() ) / maxDensity;

		t = max( sd, t );

		hitPos += t * ray.direction;
		tTotal += t;

		sd = de2( hitPos ) * 0.9f; // understep

		// if you hit
		if ( density( hitPos ) > rFloat() || tTotal > GlobalData.raymarchMaxDistance ) {
			break;
		}
	}

	return tTotal;
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
	uv *= 0.4f;
	uv.y /= aspectRatio;
	uv.x -= 0.05f;
	uv = vec2( atan( uv.y, uv.x ) + 0.5f, ( length( uv ) + 0.5f ) * acos( -1.0f ) );
	vec3 baseVec = normalize( vec3( cos( uv.y ) * cos( uv.x ), sin( uv.y ), cos( uv.y ) * sin( uv.x ) ) );
	baseVec = Rotate3D( pi / 2.0f, vec3( 2.5f, 0.4f, 1.0f ) ) * baseVec; // this is to match the other camera

	ray_t ray;
	ray.origin = GlobalData.viewerPosition;
	ray.direction = normalize( -baseVec.x * GlobalData.basisX + baseVec.y * GlobalData.basisY + ( 1.0f / GlobalData.FoV ) * baseVec.z * GlobalData.basisZ );
//	ray.direction = normalize( aspectRatio * uv.x * GlobalData.basisX + uv.y * GlobalData.basisY + ( 1.0f / GlobalData.FoV ) * GlobalData.basisZ );

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

		const float d = getSceneIntersection( ray );
		const float deltaD = deltaTrack( ray );

		if ( deltaD > GlobalData.raymarchMaxDistance && d > GlobalData.raymarchMaxDistance ) {

		// this ray has escaped the scene to the sky, so we take a sky sample + kill it
			// accumulatedRadiance += transmission * max( 3.0f * dot( ray.direction, vec3( 0.0f, 0.0f, 1.0f ) ), 0.0f );
			accumulatedRadiance += transmission * 5.0f * step( 0.8f, dot( ray.direction, vec3( 0.0f, 0.0f, -1.0f ) ) );
			break;

		} else {

		// this ray interacts with the scene
			// direct lighting contribution - tbd, transmission needs to be corrected... directional sun should be a scalar

			// first, doing a delta track raymarch to compare with the scene intersection distance

			if ( deltaD < d ) {
			// this is a volume scattering event

				transmission *= 0.99f;
				ray.origin = ray.origin + ray.direction * deltaD;
				ray.direction = sampleApproxMieDirection( ray.direction, 15, rFloat(), rFloat(), rFloat() );

			} else {
			// this is a surface scattering event
				// I want to handle materials just like Daedalus... that can happen here, using global stuff

				int SDFMaterial = hitSurfaceType;
				vec3 SDFAlbedo = hitColor;
				float SDFRough = hitRoughness;

				// generating a new ray from the intersection
				const vec3 normal = SDFNormal( ray.origin + ray.direction * d );
				ray.origin = ray.origin + ray.direction * d + 3.0f * GlobalData.epsilon * normal;

				switch ( SDFMaterial ) {
					case NOHIT: // shouldn't be hitting this
						break;

					case EMISSIVE:
						accumulatedRadiance += transmission * SDFAlbedo;
						ray.direction = cosWeightedRandomHemisphereDirection( normal );
						break;

					case DIFFUSE:
						transmission *= SDFAlbedo;
						ray.direction = cosWeightedRandomHemisphereDirection( normal );
						break;

					case METALLIC:
						transmission *= SDFAlbedo;
						ray.direction = normalize( ( 1.0f + GlobalData.epsilon ) * normal + mix( reflect( ray.direction, normal ), RandomUnitVector(), SDFRough ) );
						break;

					case MIRROR:
						transmission *= SDFAlbedo;
						ray.direction = reflect( ray.direction, normal );
						break;

					default:
						break;
				}
			}
		}
	}

	color = accumulatedRadiance;

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