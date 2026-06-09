#pragma once

#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"

#include <deque>
#include <vector>
#include <random>
#include <fstream>

#include <SDL3/SDL_mouse.h>

#include "third_party/nlohmann/json.hpp"
using json = nlohmann::json;

#include <vk_types.h>

// remap the value from [inLow..inHigh] to [outLow..outHigh]
inline float remap ( float value, float inLow, float inHigh, float outLow, float outHigh ) {
	return outLow + ( value - inLow ) * ( outHigh - outLow ) / ( inHigh - inLow );
}

using glm::vec2;
using glm::vec3;
using glm::vec4;
using glm::bvec4;

#include "spectralData/spectralToolkit.h"

inline glm::vec3 HexToVec3 ( const std::string& hex ) {
	// uint8_t r, g, b;
	// std::stringstream( hex ) >> std::hex >> r >> g >> b;
	// return glm::vec3( r / 255.0f, g / 255.0f, b / 255.0f );

	// Check if the input hex string is valid (should be 6 characters without the #)

	// Extract the RGB components from the hex string
	std::string redHex = hex.substr(0, 2);
	std::string greenHex = hex.substr(2, 2);
	std::string blueHex = hex.substr(4, 2);

	// Convert hex to integers
	int r, g, b;
	std::stringstream ss;

	ss << std::hex << redHex;
	ss >> r;
	ss.clear();

	ss << std::hex << greenHex;
	ss >> g;
	ss.clear();

	ss << std::hex << blueHex;
	ss >> b;

	// Normalize the values to 0-1 range by dividing by 255
	vec3 color;
	color.r = r / 255.0f;
	color.g = g / 255.0f;
	color.b = b / 255.0f;

	return color;
}

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_impl_vulkan.h"

#include "third_party/Jakob2019Spectral/supplement/rgb2spec.h"
//======================================================================================================================
// for handing out unique identifiers
static int uniqueID { 0 };

// memory associated with the xRite color chip reflectances
static const float** xRiteReflectances = nullptr;

// memory associated with the source PDFs
static int numSourcePDFs = 0;
static const float** sourcePDFs = nullptr;
static const char** sourcePDFLabels = nullptr;

// information about the gel filters
static int numGelFilters = 0;
static const float** gelFilters = nullptr;
static const char** gelFilterLabels = nullptr;
static const char** gelFilterDescriptions = nullptr;
static const glm::vec3* gelPreviewColors = nullptr;

// used to draw the preview images on the menu
static ImTextureID textureID;
//======================================================================================================================
struct LightEmitterParameters {
	// base emitter
	vec2 position = vec2( 0.0f, 0.0f );
	float rotation = 0.0f;

	// angular distribution
	float angleScalar = 0.0f;
	float cauchyMix = 0.0001f;

	// array modifier
	int32_t repeats = 1;
	float emitterSpacing = 0.0f;
	float width = 10.0f;
};
//======================================================================================================================
// light class -> configuration for a single light
class Light {
public:

	Light ( float brightness_in = 1.0f ) : brightness { brightness_in } {
		// ImGUI needs distinct strings... can use an int, just assign at construction time
		uniqueID += 42069;
		myUniqueID = uniqueID;

		std::mt19937 seedRNG( [] {
			std::random_device rd;
			std::seed_seq seq{  rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
			return std::mt19937( seq );
		} () );
		// random starting value
		PDFPick = std::uniform_int_distribution< int >( 0, numSourcePDFs - 1 )( seedRNG );

		Update();
	}

	// state flags for the manager
	bool dirtyFlag { false }; // need to call Update() on this light
	bool deleteFlag { false }; // need to delete this light

	LightEmitterParameters parameters;

	// need to set this in the maintenance function
	ImVec2 minUV { 0.0f, 0.0f };
	ImVec2 maxUV { 1.0f, 1.0f };

	// called inside of the light manager ImGui Draw function
	void ImGuiDrawLightInfo ( bool mouseLight = false ) {
		static std::mt19937 seedRNG( [] {
			std::random_device rd;
			std::seed_seq seq{  rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
			return std::mt19937( seq );
		} () );

		// use myUniqueID in the labels, disambiguates between otherwise identical labels for ImGui
		const std::string lString = std::string( "##" ) + std::to_string( myUniqueID );

		// spectrum preview + xrite checker
			// will just be an image you need to show using the ImGui TextureID
		ImGui::Image( textureID, ImVec2( 554, 64 ), minUV, maxUV );

		ImGui::SliderFloat( ( "Brightness" + lString ).c_str(), &brightness, 0.0001f, 100.0f, "%.5f", ImGuiSliderFlags_Logarithmic );
		if ( ImGui::IsItemEdited() ) dirtyFlag = true;

		// source PDF picker
		ImGui::Combo( ( std::string( "Light Type" ) + lString ).c_str(), &PDFPick, sourcePDFLabels, numSourcePDFs ); // may eventually do some kind of scaled gaussians for user-configurable RGB triplets...
		if ( ImGui::IsItemEdited() ) dirtyFlag = true;

		ImGui::SameLine(); // pick a random source PDF
		if ( ImGui::Button( ( "Randomize" + lString ).c_str() ) ) {
			PDFPick = std::uniform_int_distribution< int >( 0, numSourcePDFs - 1 )( seedRNG );
			dirtyFlag = true; // we changed the light, need to update
		}

		// gel filter list
		for ( int i = 0; i < filterStack.size(); i++ ) {
			ImGui::PushID( i );
			ImGui::Separator();

			// show gel picker
			ImGui::Combo( ( "Gel" + lString ).c_str(), &filterStack[ i ], gelFilterLabels, numGelFilters );
			if ( ImGui::IsItemEdited() ) dirtyFlag = true;

			ImGui::SameLine();
			if ( ImGui::Button( ( "Randomize" + lString ).c_str() ) ) {
				filterStack[ i ] = std::uniform_int_distribution< int >( 0, numGelFilters - 1 )( seedRNG );
				dirtyFlag = true;
			}

			ImGui::SameLine();
			if ( ImGui::Button( ( "Remove Gel" + lString ).c_str() ) ) {
				filterStack.erase( filterStack.begin() + i );
				dirtyFlag = true;
			} else { // need to prevent accessing these things if we remove
				// show selected gel preview color
				vec3 col = gelPreviewColors[ filterStack[ i ] ];

				if ( ImGui::ColorButton( ( "##ColorSquare" + lString ).c_str(), ImColor( col.r, col.g, col.b ), ImGuiColorEditFlags_NoAlpha, ImVec2(16, 16 ) ) ) {}
				ImGui::SameLine();

				// show selected gel description
				ImGui::TextWrapped( "%s", gelFilterDescriptions[ filterStack[ i ] ] );
			}
			ImGui::PopID();
		}

		// button to add a new gel to the stack
		if ( ImGui::Button( ( "Add Gel" + lString ).c_str() ) ) {
			filterStack.emplace_back(  std::uniform_int_distribution< int >( 0, numGelFilters - 1 )( seedRNG ) );
			dirtyFlag = true;
		}

		// option to remove -> set deleteFlag
		ImGui::PushID( uniqueID );
		if ( !mouseLight ) {
			// emitter parameters
			ImGui::Separator();
			ImGui::Text("Emitter Parameters:" );
			ImGui::SliderFloat2( ( "Location" + lString ).c_str(), ( float* ) &parameters.position, 0.0f, 2000.0f, "%.1f" );
			ImGui::SliderFloat( ( "Rotation" + lString ).c_str(), &parameters.rotation, 0.0f, 2.0f * 3.141592f, "%.3f" );
			ImGui::SliderFloat( ( "Width" + lString ).c_str(), &parameters.width, 0.0f, 1800.0f, "%.1f", ImGuiSliderFlags_Logarithmic );

			ImGui::Text( "Angular Distribution:" );
			ImGui::SliderFloat( ( "Angle" + lString ).c_str(), &parameters.angleScalar, 0.0f, 2.0f * 3.141592f, "%.3f", ImGuiSliderFlags_Logarithmic );
			ImGui::SliderFloat( ( "Cauchy Mix" + lString ).c_str(), &parameters.cauchyMix, 0.0f, 1.0f, "%.6f", ImGuiSliderFlags_Logarithmic );

			ImGui::Text( "Array Mod:" );
			ImGui::SliderInt( ( "Repeats" + lString ).c_str(), &parameters.repeats, 1, 10 );
			if ( parameters.repeats != 1 )
				ImGui::SliderFloat( ( "Spacing" + lString ).c_str(), &parameters.emitterSpacing, 0.0f, 500.0f, "%.3f", ImGuiSliderFlags_Logarithmic );

			if ( ImGui::Button( ( "Remove Light" + lString ).c_str() ) ) {
				deleteFlag = true;
			}
		} else {
			ImGui::Text("Emitter Parameters:" );
			ImGui::SliderFloat( ( "Rotation" + lString ).c_str(), &parameters.rotation, 0.0f, 2.0f * 3.141592f, "%.3f" );
			ImGui::SliderFloat( ( "Width" + lString ).c_str(), &parameters.width, 0.0f, 1800.0f, "%.1f", ImGuiSliderFlags_Logarithmic );

			ImGui::Text( "Angular Distribution:" );
			ImGui::SliderFloat( ( "Angle" + lString ).c_str(), &parameters.angleScalar, 0.0f, 2.0f * 3.141592f, "%.3f", ImGuiSliderFlags_Logarithmic );
			ImGui::SliderFloat( ( "Cauchy Mix" + lString ).c_str(), &parameters.cauchyMix, 0.0f, 1.0f, "%.6f", ImGuiSliderFlags_Logarithmic );

			ImGui::Text( "Array Mod:" );
			ImGui::SliderInt( ( "Repeats" + lString ).c_str(), &parameters.repeats, 1, 10 );
			if ( parameters.repeats != 1 )
				ImGui::SliderFloat( ( "Spacing" + lString ).c_str(), &parameters.emitterSpacing, 0.0f, 500.0f, "%.3f", ImGuiSliderFlags_Logarithmic );
		}
		ImGui::PopID();
	}

	void Update () {
		// compute the current PDF (temporary, not stored outside of this function):
		std::vector< float > PDFScratch;

	// start with the selected light PDF LUT, then apply as many gel filters as there are to apply

	// Starting from the selected source PDF, with no filters applied.
		// We need a curve out of this process representing the filtered light.

		auto LoadPDF = [&] ( int idx ) {
			PDFScratch.clear();
			for ( int i = 0; i < 450; i++ ) {
				PDFScratch.emplace_back( sourcePDFs[ idx ][ i ] );
			}
		};

		auto ApplyFilter = [&] ( int idx ) {
			for ( int i = 0; i < 450; i++ ) {
				PDFScratch[ i ] *= gelFilters[ idx ][ i ];
			}
		};

		auto NormalizePDF = [&] () {
			float max = 0.0f;
			for ( int i = 0; i < 450; i++ ) {
				// first pass, determine the maximum
				max = std::max( max, PDFScratch[ i ] );
			}
			for ( int i = 0; i < 450; i++ ) {
				// second pass we perform the normalization by the observed maximum
				PDFScratch[ i ] /= max;
			}
		};

		// load the selected source PDF
		LoadPDF( PDFPick );
		NormalizePDF();

		// for each selected gel filter
		for ( int i = 0; i < filterStack.size(); i++ ) {
			// apply selected gel filter
			ApplyFilter( filterStack[ i ] );

			// renormalize the PDF
			NormalizePDF();
		}

		// we have the final PDF, now let's get CDF and iCDF
		std::vector< float > cdf;
		float cumSum = 0.0f;
		for ( int x = 0; x < PDFScratch.size(); x++ ) {
			float sum = 0.0f;
			// increment cumulative sum and CDF
			cumSum += PDFScratch[ x ];
			cdf.push_back( cumSum );
		}

		// normalize the CDF values by the final value during CDF sweep
		std::vector< glm::vec2 > CDFpoints;
		for ( int x = 0; x < PDFScratch.size(); x++ ) {
			// compute the inverse CDF with the aid of a series of 2d points along the curve
			// adjust baseline for our desired range -> 380nm to 830nm, we have 450nm of data
			CDFpoints.emplace_back( x + 380, cdf[ x ] / cumSum );
		}

		iCDF.clear();
		iCDF.reserve( 1024 );
		for ( int x = 0; x < 1024; x++ ) {
			// each pixel along this strip needs a value of the inverse CDF
			// this is the intersection with the line defined by the set of segments in the array CDFpoints
			float normalizedPosition = ( x + 0.5f ) / 1024.0f;
			for ( int p = 0; p < CDFpoints.size(); p++ )
				if ( p == ( CDFpoints.size() - 1 ) ) {
					iCDF.emplace_back( CDFpoints[ p ].x );
				} else if ( CDFpoints[ p ].y >= normalizedPosition ) {
					iCDF.emplace_back( remap( normalizedPosition, CDFpoints[ std::max( p - 1, 0 ) ].y, CDFpoints[ p ].y + 0.0001f, CDFpoints[ std::max( p - 1, 0 ) ].x, CDFpoints[ p ].x + 0.0001f ) );
					break;
				}
		}

		// clearing the image
		for ( size_t i = 0; i < previewImageSize.width * previewImageSize.height * 4; i+= 4 ) {
			textureScratch[ i + 0 ] = 0;
			textureScratch[ i + 1 ] = 0;
			textureScratch[ i + 2 ] = 0;
			textureScratch[ i + 3 ] = 255;
		}

		// at the start here, you have the updated light PDF...
			// let's go ahead and compute the preview...
		int xOffset = 0;
		for ( auto& freqBand : PDFScratch ) {
			// we know the PDF value at this location...
			for ( size_t y = 0; y < previewImageSize.height; y++ ) {
				float fractionalPosition = 1.0f - float( y ) / float( previewImageSize.height );
				if ( fractionalPosition < freqBand ) {
					// we want to use a representative color for the frequency...
					vec3 c = wavelengthColorLDR( xOffset + 380.0f ) * 255.0f;
					setPixel( xOffset, y, c );
				} else {
					// write clear... maybe a grid pattern?
					float xWave = sin( xOffset * 0.5f );
					float yWave = sin( y * 0.5f );
					const float p = 40.0f;
					float v = std::max( 32.0f * pow( ( 16 + 15 * xWave ) / 32.0f, p ), 32.0f * pow( ( 16 + 15 * yWave ) / 32.0f, p ) );
					setPixel( xOffset, y, vec3( float( v ) ) );
				}
			}
			xOffset++;
		}

		// we know the sRGB xRite color chip reflectances, and the light emission... we need to convolve to get the color result
		vec3 color[ 24 ];
		for ( int chip = 0; chip < 24; chip++ ) {
			// initial accumulation value
			color[ chip ] = vec3( 0.0f );

			// we need to iterate over wavelengths and get an average color value under this illuminant
			for ( int y = 0; y < 450; y++ ) {
				// color[ chip ] += ( wavelengthColorLinear( 380 + y ) * light.PDFScratch[ y ] ) / 450.0f;
				color[ chip ] += 3.5f * glm::clamp( wavelengthColorLinear( 380.0f + y ) * xRiteReflectances[ chip ][ y ] * PDFScratch[ y ], vec3( 0.0f ), vec3( 1.0f ) ) / 450.0f;
			}
		}

		for ( int x = 0; x < 6; x++ ) {
			for ( int y = 0; y < 4; y++ ) {
				int i = x + 6 * y;
				int bx = 455;
				int by = 5;
				int xS = 13;
				int yS = 12;
				int xM = 2;
				int yM = 2;

				for ( int xo = 0; xo < xS; xo++ ) {
					for ( int yo = 0; yo < yS; yo++ ) {
						setPixel( bx + ( xS + xM ) * x + xo, by + ( yS + yM ) * y + yo, glm::vec3( color[ i ].r * 255, color[ i ].g * 255, color[ i ].b * 255 ) );
					}
				}
			}
		}

		// writing a preview
		/* stbi_write_png(
			std::string( "test.png" ).c_str(),
			previewImageSize.width,
			previewImageSize.height,
			4, // RGBA
			textureScratch,
			previewImageSize.width * 4 // stride (bytes per row)
		);*/

		// we have done the update
		dirtyFlag = false;
	}

	// spectral distribution
	int PDFPick{ 12 };
	std::vector< int > filterStack;

	// the solved iCDF, needed by the light manager
	std::vector< float > iCDF;

	// unitless, relative brightness
	float brightness;

	// ImGui textureID thing
	ImTextureID myTextureID;

	// this is just some dimensions I picked, based on the pixel drawings
	static constexpr VkExtent3D previewImageSize { 554, 64 };
	uint8_t textureScratch[ 4 * previewImageSize.width * previewImageSize.height ];
		// texture scratch memory, static allocation -> copied to atlas

	// helper functions for setting pixel color
	inline void setPixel ( int x, int y, const glm::vec3& color ) {
		int index = 4 * ( y * previewImageSize.width + x );

		textureScratch[ index + 0 ] = static_cast<uint8_t>( color.r );
		textureScratch[ index + 1 ] = static_cast<uint8_t>( color.g );
		textureScratch[ index + 2 ] = static_cast<uint8_t>( color.b );
		textureScratch[ index + 3 ] = 255; // constant alpha
	}

	int myUniqueID; // for ImGUI
};
//======================================================================================================================
class LightManager {
public:
	bool needsUpdate { true };
	static constexpr int maxLights { 256 };
	LightManager () {}

	float* brightnessScalar = nullptr;

	void Initialize () {
		// create the texture for the light spectrum sampling -> scale the Y for some max
		// create the texture for the light picking -> 256 max lights means R8 texture can be used for the picking texture

		// load the light PDFs
		LoadPDFData();

		// load the data about the filters -> labels, swatch colors, and spectral filter %Y vectors
		LoadGelFilterData();

		// load the data for the sRGB reflectances
		PrecomputesRGBReflectances();

		// need to call this to prepare
		MouseLight = std::make_unique<Light>();

		// and add one placeholder user light
		// lights.emplace_back();
	}

	// get rid of all lights
	void clearList () {
		lights.clear();
	}

	// you always have a mouse light
	glm::vec2 MouseLocation { 0.0f };
	std::unique_ptr<Light> MouseLight = nullptr;
	std::deque< Light > lights;

	// staging memory for the light emitter parameter buffer
	LightEmitterParameters lightEmitterParameters[ maxLights ];

	void ImGuiDrawLightList () {
		// configuration for the mouse light
		ImGui::Separator();
		ImGui::Text( "Mouse Light" );
		ImGui::Separator();
		static bool mouseLightVisible = true;
		if ( ImGui::CollapsingHeader( "Show/Hide", &mouseLightVisible, ImGuiTreeNodeFlags_DefaultOpen ) ) {
			MouseLight->ImGuiDrawLightInfo( true );
		}

		// something to make it stand out from the user light list:
		ImGui::Separator();
		ImGui::Text( "User Lights (max 255)" );
		ImGui::Separator();

		// and then for a growable list of lights after that
		for ( auto & light : lights ) {
			if ( ImGui::CollapsingHeader( ( "Show/Hide##" + std::to_string( light.myUniqueID ) ).c_str() ) ) {
				light.ImGuiDrawLightInfo();
			}
		}

		// optionally add a light to the list:
		if ( ImGui::Button( "Add Light" ) ) {
			AddLight();
		}

	// after the list is drawn...
		// iterate through and Update() any with the dirtyFlag
		for ( auto & light : lights ) {
			if ( light.dirtyFlag ) {
				light.Update();
				needsUpdate = true;
			}
		}
		// same for the mouse light
		if ( MouseLight->dirtyFlag ) {
			MouseLight->Update();
			needsUpdate = true;
		}

		// delete any that shouldn't exist anymore
		lights.erase( std::remove_if( lights.begin(), lights.end(),
			[ & ]( const auto& light ) {
				if ( light.deleteFlag ) {
					needsUpdate = true;
					return true;
				}
				return false;
			}), lights.end()
		);
	}

	void AddLight ( float brightness = 1.0f ) {
		// calculate the brightness increment needed to keep constant...
		float prevSumPower{ MouseLight->brightness };
		for ( auto & light : lights ) {
			prevSumPower += light.brightness;
		}

		lights.emplace_back( brightness ); // constructor calls Update()
		needsUpdate = true;

		// should keep subjective brightness constant (light initialized with brightess = 1)
		*brightnessScalar = *brightnessScalar * ( ( prevSumPower + brightness ) / prevSumPower );
	}

	void MouseLightToUserLight () {

		// add a new light, at the mouse brightness
		AddLight( MouseLight->brightness );

		// other parameters come from the mouse parameters
		lights.back().parameters = MouseLight->parameters;

		// position is the mouse position
		static float mouseX, mouseY;
		SDL_GetMouseState( &mouseX, &mouseY );
		lights.back().parameters.position.x = mouseX;
		lights.back().parameters.position.y = mouseY;

		// light distribution comes from the mouse distribution
		lights.back().PDFPick = MouseLight->PDFPick;
		for ( auto & filter : MouseLight->filterStack ) {
			lights.back().filterStack.emplace_back( filter );
		}

		lights.back().Update();
	}

// we have two different importance sampling structures...
	// first is a list of the light spectral iCDFs, in a texture
	std::vector< float > iCDFTexture; // 1024 floats defines one iCDF

	// second is for preferentially picking the lights by brightness
	std::vector< uint8_t > pickTexture;

	// and each light has prepared a spectrum preview -> need to combine into atlas
	std::vector< uint8_t > concatenatedPreviews;

	uint32_t numLights{ 0 };

	// this will need to happen any time we have an edit
	void Update () {
		numLights = lights.size() + 1; // user lights + mouse light

		// construct the light spectral sampling texture from light iCDFs
			// data comes from each light, the data should be available by the time this runs
		iCDFTexture.resize( numLights * 1024 );
		for ( size_t i = 0; i < 1024; i++ ) {
			iCDFTexture[ i ] = MouseLight->iCDF[ i ];
		}

		// the user lights do the same
		for ( size_t j = 1; j < numLights; j++ ) {
			for ( size_t i = 0; i < 1024; i++ ) {
				iCDFTexture[ j * 1024 + i ] = lights[ j - 1 ].iCDF[ i ];
			}
		}

		// construct the light pick texture from the light brightnesses
			// need to refer to individual light brightnesses relative to the sum of all brightnesses in the list
		std::vector< float > brightnesses;
		brightnesses.emplace_back( MouseLight->brightness );
		for ( auto & light : lights ) {
			brightnesses.emplace_back( light.brightness );
		}

		// size of the texture doesn't actually matter on the GPU side, since it will use normalized sampling
		glm::vec2 isSize = glm::vec2( 256, 256 );

		// for RNG
		static std::mt19937 seedRNG( [] {
			// RNG ( mostly for generating GPU-side RNG seed)
			std::random_device rd;
			std::seed_seq seq{  rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
			return std::mt19937( seq );
		} () );

		// get some samples from this process (std::random has a good tool for this) -> this is the texture memory
		pickTexture.resize( isSize.x * isSize.y );
		std::discrete_distribution<> d({ brightnesses.begin(), brightnesses.end() });
		for ( size_t i = 0; i < isSize.x * isSize.y; i++ ) {
			pickTexture[ i ] = d( seedRNG );
		}

		// next we need to make sure that the atlas is constructed
		int numValuesPerPreview = 554 * 64 * 4;
		concatenatedPreviews.resize( numValuesPerPreview * numLights );
		for ( int i = 0; i < numValuesPerPreview; i++ ) {
			concatenatedPreviews[ i ] = MouseLight->textureScratch[ i ];
		}
		for ( int light = 0; light < numLights - 1; light++ ) {
			for ( int i = 0; i < numValuesPerPreview; i++ ) {
				concatenatedPreviews[ ( light + 1 ) * numValuesPerPreview + i ] = lights[ light ].textureScratch[ i ];
			}
		}

		// updating atlas positions
		MouseLight->minUV = ImVec2( 0.0f, 0.0f );
		MouseLight->maxUV = ImVec2( 1.0f, 1.0f / numLights );
		for ( int light = 0; light < numLights - 1; light++ ) {
			lights[ light ].minUV = ImVec2( 0.0f, ( light + 1 ) * 1.0f / numLights );
			lights[ light ].maxUV = ImVec2( 1.0f, ( light + 2 ) * 1.0f / numLights );
		}

		// construct the buffer for the light parameters
		int idx = 0;
		lightEmitterParameters[ idx ] = MouseLight->parameters;
		while ( ( idx + 1 ) < lights.size() ) {
			idx++;
			lightEmitterParameters[ idx ] = lights[ idx ].parameters;
		}

		// update complete
		needsUpdate = false;
	}

private:
	void PrecomputesRGBReflectances () {
		// populating the xRite color checker card
		const vec3 sRGBConstants[] = {
			vec3( 115,  82,  68 ), // dark skin
			vec3( 194, 150, 120 ), // light skin
			vec3(  98, 122, 157 ), // blue sky
			vec3(  87, 108,  67 ), // foliage
			vec3( 133, 128, 177 ), // blue flower
			vec3( 103, 189, 170 ), // bluish green
			vec3( 214, 126,  44 ), // orange
			vec3(  80,  91, 166 ), // purplish blue
			vec3( 193,  90,  99 ), // moderate red
			vec3(  94,  60, 108 ), // purple
			vec3( 157, 188,  64 ), // yellow green
			vec3( 244, 163,  46 ), // orange yellow
			vec3(  56,  61, 150 ), // blue
			vec3(  70, 148,  73 ), // green
			vec3( 175,  54,  60 ), // red
			vec3( 231, 199,  31 ), // yellow
			vec3( 187,  86, 149 ), // magenta
			vec3(   8, 133, 161 ), // cyan
			vec3( 243, 243, 242 ), // white
			vec3( 200, 200, 200 ), // neutral 8
			vec3( 160, 160, 160 ), // neutral 6.5
			vec3( 122, 122, 121 ), // neutral 5
			vec3(  85,  85,  85 ), // neutral 3.5
			vec3(  52,  52,  52 ) // black
		};

	// there is some resources associated with this sampling process...
		// we first need to load the LUT from Jakob's paper
		// https://rgl.epfl.ch/publications/Jakob2019Spectral
		RGB2Spec *model = rgb2spec_load( "../src/third_party/Jakob2019Spectral/supplement/tables/srgb.coeff" );

		// once we have that, we can use an sRGB constant to derive a reflectance curve
			// we are going to do that by 1nm bands to match the other data
		xRiteReflectances = ( const float ** ) malloc( 24 * sizeof( float * ) );

		// for each of the reflectances
		for ( int i = 0; i < 24; i++ ) {
			// first step get the reflectance coefficients
			float rgb[ 3 ] = { sRGBConstants[ i ].r / 255.0f, sRGBConstants[ i ].g / 255.0f, sRGBConstants[ i ].b / 255.0f }, coeff[ 3 ];

			rgb2spec_fetch( model, rgb, coeff );
			// printf( "fetch(): %f %f %f\n", coeff[ 0 ], coeff[ 1 ], coeff[ 2 ] );

			// allocate and populate the reflectance corresponding to this color chip
			xRiteReflectances[ i ] = ( const float * ) malloc( 450 * sizeof( float ) );
			for ( int l = 0; l < 450; l++ ) {
				( float& ) xRiteReflectances[ i ][ l ] = rgb2spec_eval_precise( coeff, float( l + 380 ) );
			}
		}
	}

	void LoadPDFData () {
		// setup the texture with rows for the specific light types, for the importance sampled emission spectra
		const std::string LUTPath = "../src/spectralData/LightPDFs/";

		// need to populate the array of LUT filenames
		if ( std::filesystem::exists( LUTPath ) && std::filesystem::is_directory( LUTPath ) ) {
			// Iterate over the directory contents
			std::vector< std::filesystem::path > paths;
			for ( const auto& entry : std::filesystem::directory_iterator( LUTPath ) ) {
				// Check if the entry is a regular file
				if ( std::filesystem::is_regular_file( entry.status() ) ) {
					paths.push_back( entry.path() );
					// cout << "adding " << entry.path().filename().stem() << endl;
				}
			}

			// we have a list of filenames, now we need to create the buffers to hold the data + labels
			sourcePDFs = ( const float ** ) malloc( paths.size() * sizeof( const float * ) );
			sourcePDFLabels = ( const char ** ) malloc( paths.size() * sizeof( const char * ) );

			for ( size_t i = 0u; i < paths.size(); i++ ) {
				// populate the labels
				sourcePDFLabels[ i ] = ( const char * ) malloc( strlen( paths[ i ].filename().stem().string().c_str() ) + 1 );
				strcpy( ( char * ) sourcePDFLabels[ i ], paths[ i ].filename().stem().string().c_str() );

				// we need to process each of the source distributions into a PDF
				sourcePDFs[ i ] = ( const float * ) malloc( 450 * sizeof( float ) );

				// helper for the below
				const auto getLuma = [] ( const glm::vec3& v ) -> float {
					const float scaleFactors[ 3 ] = { 0.299f, 0.587f, 0.114f };
					float sum = 0.0f;
					for ( int c = 0; c < 3; ++c ) sum += v[ c ] * v[ c ] * scaleFactors[ c ];
					return std::sqrt( sum );
				};

			// loading the image needs to change to use stb_image
				int width, height, channels;
				unsigned char *data = stbi_load( paths[ i ].string().c_str(), &width, &height, &channels, 0 );
				if ( data == NULL ) {
					printf("Failed to load image: %s\n", stbi_failure_reason());
				}

				// load the referenced data to decode the emission spectra PDF
				for ( int x = 0; x < width; x++ ) {
					float sum = 0.0f;
					for ( int y = 0; y < height; y++ ) {
						vec3 col = vec3( 0.0f, 0.0f, 0.0f );
						col.r = data[ ( x + width * y ) * channels ] / 255.0f;
						col.g = data[ ( x + width * y ) * channels + 1 ] / 255.0f;
						col.b = data[ ( x + width * y ) * channels + 2 ] / 255.0f;
						sum += 1.0f - getLuma( col );
					}
					( float& ) sourcePDFs[ i ][ x ] = sum;
				}

				// and increment count
				numSourcePDFs++;

				/*
				// and the debug dump
				cout << "adding source distribution: " << endl << sourcePDFLabels[ i ] << endl;
				for ( size_t x = 0; x < pdfLUT.Width(); x++ ) {
					cout << " " << sourcePDFs[ i ][ x ];
				}
				cout << endl << endl;
				*/
			}

		} else {
			std::cerr << "Directory does not exist or is not a directory." << std::endl;
		}
	}

	void LoadGelFilterData () {
		// loading the initial data from the JSON records
		json gelatinRecords;
		std::ifstream i( "../src/spectralData/LeeGelList.json" );
		i >> gelatinRecords; i.close();

		struct gelRecord {
			std::string label;
			std::string description;
			vec3 previewColor;
			std::vector< float > filterData;
		};

		std::vector< gelRecord > gelRecords;

		// iterating through and finding all the gel filters that have nonzero ("valid") spectral data
		for ( auto& e : gelatinRecords ) {
			// getting the data we need... problem is we don't know ahead of time, how many there are

			// Need to do some processing to separate label and description
			std::string text = e[ "text" ];
			size_t firstPos = text.find_first_not_of( " \n" );
			size_t numEnd = text.find( ' ', firstPos );
			std::string number = text.substr( firstPos, numEnd - firstPos );
			size_t secondPos = text.find( number, numEnd );

			// also some processing in anticipation of needing the color, too
			std::string c = e[ "color" ];
			std::transform( c.begin(), c.end(), c.begin(), [] ( unsigned char cf ) { return std::tolower( cf ); } );
			vec3 color = HexToVec3( c );

			// going through the filter is where we will be able to determine if this entry is valid or not
			bool valid = true;
			std::vector< float > filter;
			std::vector< float > filterScratch;
			filter.clear();
			if ( e.contains( "datatext" ) ) {
				for ( int lambda = 405;; lambda += 5 ) {
					if ( e[ "datatext" ].contains( std::to_string( lambda ) ) ) {
						filter.push_back( std::stof( e[ "datatext" ][ std::to_string( lambda ) ].get< std::string >() ) / 100.0f );
					} else {
						// loop exit
						if ( filter.size() != 0 )
							filter.push_back( filter[ filter.size() - 1 ] );
						break;
					}
				}
			}

			// we want to dismiss under two conditions:
				// zero length filter (filter data was not included)
				// filter is all zeroes (filter data was replaced with placeholder)
			if ( filter.size() != 0 ) {
				// let's also determine that we have valid coefficients:
				bool allZeroes = true;
				for ( int f = 0; f < filter.size(); f++ ) {
					if ( filter[ f ] != 0.0f ) {
						allZeroes = false;
					}
				}
				// all zeroes means invalid
				if ( allZeroes ) {
					valid = false;
				} else {
					// great - we have valid filter data...
						// let's pad out the edges and interpolate from 400-700 by 5's to 380-830 by 1's for the engine

					// low side pad with value in index 0 (optionally you could make this 1's or 0's, as desired)
					for ( int w = 380; w < 400; w++ ) {
						filterScratch.push_back( filter[ 0 ] );
					}

					// interpolate the middle section, 400-700nm
					float vprev = filter[ 0 ];
					float v = filter[ 1 ];
					for ( int wOffset = 1; wOffset < filter.size(); wOffset++ ) {
						// each entry spawns 5 elements
						for ( int j = 0; j < 5; j++ ) {
							filterScratch.push_back( glm::mix( vprev, v, ( j + 0.5f ) / 5.0f ) );
						}
						// cycle in the new values
						vprev = v;
						if ( wOffset < filter.size() ) {
							v = filter[ wOffset ];
						}
					}

					// high side pad with value in final index (also optionally force 1's or 0's, if you want)
					while ( filterScratch.size() < 450 ) {
						filterScratch.push_back( filter[ filter.size() - 1 ] );
					}
				}
			} else {
				// empty filter means invalid
				valid = false;
			}

			if ( valid ) {
				gelRecord g;

				// we can split now labels and description strings
				g.label = text.substr( firstPos, secondPos - firstPos );
				g.description = text.substr( secondPos );

				// also want to extract color from the hex codes
				g.previewColor = color;

				// and of course the filter data
				g.filterData = filterScratch;

				// we are going to collect these together to make it easier to sort
				numGelFilters++;
				gelRecords.push_back( g );
			}
		}

		// we have now constructed a list of filter datapoints... sort by labels, so we have basically ascending color codes
		std::sort( gelRecords.begin(), gelRecords.end(), [] ( gelRecord g1, gelRecord g2 ) { return g1.label < g2.label; } );

		// now we need to do the allocations for the menus - this is separated out for labels and descriptions, preview colors and filter coefficients
		gelFilters = ( const float ** ) malloc( numGelFilters * sizeof( const float * ) );
		gelFilterLabels = ( const char ** ) malloc( numGelFilters * sizeof( const char * ) );
		gelFilterDescriptions = ( const char ** ) malloc( numGelFilters * sizeof( const char * ) );
		gelPreviewColors = ( const vec3* ) malloc( numGelFilters * sizeof( const vec3 ) );

		for ( size_t i = 0; i < numGelFilters; i++ ) {
			// Allocate memory for each string and copy it
			gelFilterLabels[ i ] = ( const char * ) malloc( strlen( gelRecords[ i ].label.c_str() ) + 1 );
			strcpy( ( char * ) gelFilterLabels[ i ], gelRecords[ i ].label.c_str() );  // Copy the string

			gelFilterDescriptions[ i ] = ( const char * ) malloc( strlen( gelRecords[ i ].description.c_str() ) + 1 );
			strcpy( ( char * ) gelFilterDescriptions[ i ], gelRecords[ i ].description.c_str() );

			// just do the sRGB convert and avoid doing it every frame
			vec4 sRGB = vec4( gelRecords[ i ].previewColor[ 0 ], gelRecords[ i ].previewColor[ 1 ], gelRecords[ i ].previewColor[ 2 ], 255 );
			bvec4 cutoff = lessThan( sRGB, vec4( 0.04045f ) );
			vec4 higher = pow( ( sRGB + vec4( 0.055f ) ) / vec4( 1.055f ), vec4( 2.4f ) );
			vec4 lower = sRGB / vec4( 12.92f );
			gelRecords[ i ].previewColor =  mix( higher, lower, cutoff );
			( vec3& ) gelPreviewColors[ i ] = gelRecords[ i ].previewColor;

			// filter coefficients slightly more
			gelFilters[ i ] = ( const float * ) malloc( 450 * sizeof( const float ) );
			for ( size_t j = 0; j < 450; j++ ) {
				( float& ) gelFilters[ i ][ j ] = gelRecords[ i ].filterData[ j ];
			}

			/*
			// debug dump would be useful here
			cout << "Created Record: " << endl << gelFilterLabels[ i ] << endl;
			cout << gelFilterDescriptions[ i ] << endl;
			cout << to_string( gelPreviewColors[ i ] ) << endl;
			for ( size_t j = 0; j < 450; j++ ) {
				cout << gelFilters[ i ][ j ];
			}
			cout << endl << endl;
			*/
		}
	}
};