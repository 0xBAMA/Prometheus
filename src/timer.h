#ifndef TIMER
#define TIMER

#include <chrono>
#include <sstream>
#include <iostream>
#include <algorithm>

#include <math.h>

using std::cout;
using std::endl;
using std::string;

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_pipelines.h>
#include <vk_loader.h>

inline std::string timeDateString () {
	auto now = std::chrono::system_clock::now();
	auto inTime_t = std::chrono::system_clock::to_time_t( now );
	std::stringstream ssA;
	ssA << std::put_time( std::localtime( &inTime_t ), "%Y-%m-%d at %H-%M-%S" );
	return ssA.str();
}

inline std::string fixedWidthTimeString () {
	auto now = std::chrono::system_clock::now();
	auto inTime_t = std::chrono::system_clock::to_time_t( now );
	std::stringstream ssA;
	ssA << std::put_time( std::localtime( &inTime_t ), "[%H:%M:%S]" );
	return ssA.str();
}

//=============================================================================
//==== OpenGL Timer Query Wrapper =============================================
//=============================================================================
struct queryPair_GPU {
	queryPair_GPU ( string s ) : label( s ) {}
	string label;
	int tStampStart = -1;
	int tStampEnd = -1;
	float result = 0.0f;
};

struct queryPair_CPU {
	queryPair_CPU ( string s ) : label( s ) {}
	string label;
	std::chrono::time_point< std::chrono::system_clock > tStart;
	std::chrono::time_point< std::chrono::system_clock > tStop;
	float result = 0.0f;
};

struct timingResult_t {
	string label;
	float tStartCPU, tStopCPU;
	float tStartGPU, tStopGPU;
};

class timerManager_t {
public:
	// storing the queries
	std::vector < queryPair_GPU > queries_GPU;
	std::vector < queryPair_CPU > queries_CPU;

	// last frame's data, prepped for output
	std::vector < timingResult_t > timingResults;

	// Vulkan handles required for timestamps...
	VkDevice * device;
	VkQueryPool * pool;
	VkCommandBuffer * cmd;

	// nanosecond division
	int timestampPeriod;

	// need this value in one place, so that queries can use it
	int currentIndex = 0;

	// t0 is timestamp for the frame's beginning
	std::chrono::time_point< std::chrono::system_clock > t0;

	// memory for storing the result from Vulkan
	static constexpr int maxQueries = 128;
	std::array< uint64_t, maxQueries * 2 > timestampBuffer {};

	void reset () {
		// clear the query lists + reserve max available storage
		queries_GPU.clear(); queries_GPU.reserve( maxQueries );
		queries_CPU.clear(); queries_CPU.reserve( maxQueries );

		// reset indexing
		currentIndex = 0;

		// reset the zero point for this frame
		t0 = std::chrono::system_clock::now();

		// and also reset the pool
		vkCmdResetQueryPool( *cmd, *pool, 0, static_cast< uint32_t >( maxQueries ) );
	}

	void gather () {
		// get the timestamp data back from Vulkan
		vkGetQueryPoolResults( *device, *pool, 0, maxQueries,
			2 * maxQueries * sizeof( uint64_t ), &timestampBuffer, 2 * sizeof( uint64_t ),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );

		// calculate the floating point deltas in ms (CPU + GPU)
		timingResults.clear();
		timingResults.reserve( currentIndex );
		float GPUOffset = 0.0f;
		for ( int i = 0; i < queries_GPU.size(); i++ ) {
			timingResult_t t;
			t.label = queries_GPU[ i ].label;

			// GPU timing
			t.tStartGPU = GPUOffset;
			GPUOffset += float( timestampBuffer[ 2 * queries_GPU[ i ].tStampEnd ] - timestampBuffer[ 2 * queries_GPU[ i ].tStampStart ] ) * ( timestampPeriod / 1000000.0f );
			t.tStopGPU = GPUOffset;

			// CPU timing
			t.tStartCPU = std::chrono::duration_cast<std::chrono::microseconds>(  queries_CPU[ i ].tStart - t0 ).count() / 1000.0f;
			t.tStopCPU = std::chrono::duration_cast<std::chrono::microseconds>(  queries_CPU[ i ].tStop - t0 ).count() / 1000.0f;

			// fmt::print( "{} runs {}ms to {}ms (CPU) and {}ms to {}ms (GPU)\n", t.label, t.tStartCPU, t.tStopCPU, t.tStartGPU, t.tStopGPU );
			timingResults.push_back( t );
		}
		// fmt::print( "\n\n" );
	}
};

// this needs to be available globally
inline timerManager_t* timerManager;

// basic tick/tock timer
class unscopedTimer {
public:
	queryPair_GPU q;
	queryPair_CPU c;

	string s;
	bool disableGPU;

	unscopedTimer ( string label, bool _disableGPU = false ) : q( label ), c( label ), s( label ), disableGPU( _disableGPU ) {}

	void tick () { // explicitly start the timer / insert initial timestamp
		// GPU timestamp
		if ( !disableGPU ) {
			int idx = timerManager->currentIndex;
			vkCmdWriteTimestamp( * ( timerManager->cmd ), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, * ( timerManager->pool ), idx );
			q.tStampStart = idx;
			timerManager->currentIndex++;
		}

		// CPU timestamp
		c.tStart = std::chrono::system_clock::now();
	}

	void tock () { // explicitly end the timer / insert final timestamp
		// GPU timestamp
		if ( !disableGPU ) {
			int idx = timerManager->currentIndex;
			vkCmdWriteTimestamp( * ( timerManager->cmd ), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, * ( timerManager->pool ), idx );
			q.tStampEnd = idx;
			timerManager->currentIndex++;
		}

		// CPU timestamp
		c.tStop = std::chrono::system_clock::now();
	}
};


// better design to just reuse the code
class scopedTimer {
public:
	unscopedTimer t;
	scopedTimer ( string label ) : t( label ) {
		t.tick();
	}
	~scopedTimer () {
		t.tock();

		// GPU query finish
		timerManager->queries_GPU.emplace_back( t.q );
		timerManager->queries_CPU.emplace_back( t.c );
	}
};

#endif
