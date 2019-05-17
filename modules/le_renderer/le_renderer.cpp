#include "pal_api_loader/ApiRegistry.hpp"

#include "le_renderer/le_renderer.h"

#include "le_backend_vk/le_backend_vk.h"
#include "le_swapchain_vk/le_swapchain_vk.h"

#include <iostream>
#include <iomanip>
#include <chrono>

#include "util/enkiTS/TaskScheduler.h"

using NanoTime = std::chrono::time_point<std::chrono::high_resolution_clock>;

#ifndef LE_RENDERER_MULTITHREADED
#	define LE_RENDERER_MULTITHREADED false
#endif

// ----------------------------------------------------------------------

struct FrameData {

	enum class State : int64_t {
		eFailedClear    = -4,
		eFailedDispatch = -3,
		eFailedAcquire  = -2,
		eInitial        = -1,
		eCleared        = 0,
		eAcquired,
		eRecorded,
		eProcessed,
		eDispatched,
	};

	struct Meta {
		NanoTime time_acquire_frame_start;
		NanoTime time_acquire_frame_end;

		NanoTime time_process_frame_start;
		NanoTime time_process_frame_end;

		NanoTime time_record_frame_start;
		NanoTime time_record_frame_end;

		NanoTime time_dispatch_frame_start;
		NanoTime time_dispatch_frame_end;
	};

	State state = State::eInitial;

	le_rendergraph_o *rendergraph = nullptr;

	size_t frameNumber = size_t( ~0 );
	Meta   meta;
};

// ----------------------------------------------------------------------

struct le_renderer_o {

	uint64_t      swapchainDirty = false;
	le_backend_o *backend        = nullptr; // Owned, created in setup

	std::vector<FrameData>  frames;
	size_t                  numSwapchainImages = 0;
	size_t                  currentFrameNumber = size_t( ~0 ); // ever increasing number of current frame
	le_swapchain_settings_t swapchain_settings{};              // default swapchain settings

	enki::TaskScheduler g_TS = {};
};

static void renderer_clear_frame( le_renderer_o *self, size_t frameIndex ); // ffdecl

// ----------------------------------------------------------------------

static le_renderer_o *
renderer_create() {
	auto obj = new le_renderer_o();

	if ( LE_RENDERER_MULTITHREADED ) {
		obj->g_TS.Initialize( 4 );
	}
	return obj;
}

// ----------------------------------------------------------------------

static void renderer_destroy( le_renderer_o *self ) {

	using namespace le_renderer; // for rendergraph_i

	const auto &lastIndex = self->currentFrameNumber;

	for ( size_t i = 0; i != self->frames.size(); ++i ) {
		auto index = ( lastIndex + i ) % self->frames.size();
		renderer_clear_frame( self, index );
		// -- FIXME: delete graph builders which we added in create
		// this is not elegant.
		rendergraph_i.destroy( self->frames[ index ].rendergraph );
	}

	self->frames.clear();

	if ( self->backend ) {
		// Destroy the backend, as it is owned by the renderer
		using namespace le_backend_vk;
		vk_backend_i.destroy( self->backend );
		self->backend = nullptr;
	}

	delete self;
}

// ----------------------------------------------------------------------
/// \brief declare a shader module which can be used to create a pipeline
/// \returns a shader module handle, or nullptr upon failure
static le_shader_module_o *renderer_create_shader_module( le_renderer_o *self, char const *path, const LeShaderStageEnum &moduleType ) {
	using namespace le_backend_vk;
	return vk_backend_i.create_shader_module( self->backend, path, moduleType );
}

// ----------------------------------------------------------------------

static le_backend_o *renderer_get_backend( le_renderer_o *self ) {
	return self->backend;
}

// ----------------------------------------------------------------------

static le_pipeline_manager_o *renderer_get_pipeline_manager( le_renderer_o *self ) {
	using namespace le_backend_vk;
	return vk_backend_i.get_pipeline_cache( self->backend );
}

// ----------------------------------------------------------------------

static void renderer_setup( le_renderer_o *self, le_renderer_settings_t const &settings ) {

	// We store swapchain settings with the renderer so that we can pass
	// backend a permanent pointer to it.
	self->swapchain_settings = settings.swapchain_settings;

	{
		// Set up the backend

		using namespace le_backend_vk;
		self->backend = vk_backend_i.create();

		le_backend_vk_settings_t backend_settings{};
		backend_settings.pWindow             = settings.window;
		backend_settings.pSwapchain_settings = &self->swapchain_settings;

		{
			// TODO: If needed, any additional renderer modules which
			// request certain extensions need to be queried here,
			// so that the list of requested extensions for backend
			// may be appended.

			backend_settings.requestedExtensions    = nullptr;
			backend_settings.numRequestedExtensions = 0;
		}

		vk_backend_i.setup( self->backend, &backend_settings );
	}

	using namespace le_backend_vk;

	// Since backend setup implicitly sets up the swapchain,
	// we may now query the available number of swapchain images.
	self->numSwapchainImages = vk_backend_i.get_num_swapchain_images( self->backend );

	using namespace le_renderer; // for rendergraph_i
	self->frames.reserve( self->numSwapchainImages );

	for ( size_t i = 0; i != self->numSwapchainImages; ++i ) {
		auto frameData        = FrameData();
		frameData.rendergraph = rendergraph_i.create();
		self->frames.push_back( std::move( frameData ) );
	}

	self->currentFrameNumber = 0;
}

// ----------------------------------------------------------------------

static void renderer_clear_frame( le_renderer_o *self, size_t frameIndex ) {

	auto &frame = self->frames[ frameIndex ];

	using namespace le_backend_vk; // for vk_bakend_i
	using namespace le_renderer;   // for rendergraph_i

	if ( frame.state == FrameData::State::eCleared ) {
		return;
	}

	// ----------| invariant: frame was not yet cleared

	// + ensure frame fence has been reached
	if ( frame.state == FrameData::State::eDispatched ||
	     frame.state == FrameData::State::eFailedDispatch ||
	     frame.state == FrameData::State::eFailedClear ) {

		while ( false == vk_backend_i.poll_frame_fence( self->backend, frameIndex ) ) {
			// Note: this call may block until the fence has been reached.
		};

		bool result = vk_backend_i.clear_frame( self->backend, frameIndex );

		if ( result != true ) {
			frame.state = FrameData::State::eFailedClear;
			return;
		}
	}

	rendergraph_i.reset( frame.rendergraph );

	//	std::cout << "CLEAR FRAME " << frameIndex << std::endl
	//	          << std::flush;

	frame.state = FrameData::State::eCleared;
}

// ----------------------------------------------------------------------

static void renderer_record_frame( le_renderer_o *self, size_t frameIndex, le_render_module_o *module_, size_t frameNumber ) {

	// High-level
	// - resolve rendergraph: which render passes do contribute?
	// - consolidate resources, synchronisation for resources
	// - For each render pass, call renderpass' render method, build intermediary command lists

	auto &frame       = self->frames[ frameIndex ];
	frame.frameNumber = frameNumber;

	if ( frame.state != FrameData::State::eCleared && frame.state != FrameData::State::eInitial ) {
		return;
	}

	// ---------| invariant: Frame was previously acquired successfully.

	frame.meta.time_record_frame_start = std::chrono::high_resolution_clock::now();

	// - build up dependencies for graph, create table of unique resources for graph

	// setup passes calls `setup` callback on all passes - this initalises virtual resources,
	// and stores their descriptors (information needed to allocate physical resources)
	//
	using namespace le_renderer; // for render_module_i, rendergraph_i
	render_module_i.setup_passes( module_, frame.rendergraph );

	// find out which renderpasses contribute, only add contributing render passes to
	// frameBuilder
	rendergraph_i.build( frame.rendergraph );

	// Execute callbacks into main application for each render pass,
	// build command lists per render pass in intermediate, api-agnostic representation
	//
	rendergraph_i.execute( frame.rendergraph, frameIndex, self->backend );

	frame.meta.time_record_frame_end = std::chrono::high_resolution_clock::now();

	frame.state = FrameData::State::eRecorded;
	// std::cout << "renderer_record_frame: " << std::dec << std::chrono::duration_cast<std::chrono::duration<double, std::milli>>( frame.meta.time_record_frame_end - frame.meta.time_record_frame_start ).count() << "ms" << std::endl;

	//	std::cout << "RECORD FRAME " << frameIndex << std::endl
	//	          << std::flush;
}

// ----------------------------------------------------------------------

static const FrameData::State &renderer_acquire_backend_resources( le_renderer_o *self, size_t frameIndex ) {

	using namespace le_backend_vk; // for vk_bakend_i
	using namespace le_renderer;   // for rendergraph_i

	// ---------| invariant: There are frames to process.

	auto &frame = self->frames[ frameIndex ];

	frame.meta.time_acquire_frame_start = std::chrono::high_resolution_clock::now();

	if ( frame.state != FrameData::State::eRecorded ) {
		return frame.state;
	}

	// ----------| invariant: frame is either initial, or cleared.

	le_renderpass_o **passes          = nullptr;
	size_t            numRenderPasses = 0;

	rendergraph_i.get_passes( frame.rendergraph, &passes, &numRenderPasses );

	le_resource_handle_t const *declared_resources;
	le_resource_info_t const *  declared_resources_infos;
	size_t                      declared_resources_count = 0;

	rendergraph_i.get_declared_resources( frame.rendergraph, &declared_resources, &declared_resources_infos, &declared_resources_count );

	auto acquireSuccess = vk_backend_i.acquire_physical_resources( self->backend, frameIndex, passes, numRenderPasses,
	                                                               declared_resources, declared_resources_infos, declared_resources_count );

	frame.meta.time_acquire_frame_end = std::chrono::high_resolution_clock::now();

	if ( acquireSuccess ) {
		frame.state = FrameData::State::eAcquired;
		//		std::cout << "ACQU FRAME " << frameIndex << std::endl
		//		          << std::flush;

	} else {
		frame.state = FrameData::State::eFailedAcquire;
		// Failure most likely means that the swapchain was reset,
		// perhaps because of window resize.
		std::cout << "WARNING: Could not acquire frame." << std::endl;
		self->swapchainDirty = true;
	}

	return frame.state;
}

// ----------------------------------------------------------------------

static const FrameData::State &renderer_process_frame( le_renderer_o *self, size_t frameIndex ) {

	using namespace le_backend_vk; // for vk_bakend_i

	auto &frame = self->frames[ frameIndex ];

	if ( frame.state != FrameData::State::eAcquired ) {
		return frame.state;
	}
	// ---------| invariant: frame was previously recorded successfully

	frame.meta.time_process_frame_start = std::chrono::high_resolution_clock::now();

	// translate intermediate draw lists into vk command buffers, and sync primitives

	vk_backend_i.process_frame( self->backend, frameIndex );

	frame.meta.time_process_frame_end = std::chrono::high_resolution_clock::now();
	//std::cout << "renderer_process_frame: " << std::dec << std::chrono::duration_cast<std::chrono::duration<double,std::milli>>(frame.meta.time_process_frame_end-frame.meta.time_process_frame_start).count() << "ms" << std::endl;

	//	std::cout << "PROCE FRAME " << frameIndex << std::endl
	//	          << std::flush;

	frame.state = FrameData::State::eProcessed;
	return frame.state;
}

// ----------------------------------------------------------------------

static void renderer_dispatch_frame( le_renderer_o *self, size_t frameIndex ) {

	using namespace le_backend_vk; // for vk_backend_i
	auto &frame = self->frames[ frameIndex ];

	if ( frame.state != FrameData::State::eProcessed ) {
		return;
	}

	// ---------| invariant: frame was successfully processed previously

	frame.meta.time_dispatch_frame_start = std::chrono::high_resolution_clock::now();

	bool dispatchSuccessful = vk_backend_i.dispatch_frame( self->backend, frameIndex );

	frame.meta.time_dispatch_frame_end = std::chrono::high_resolution_clock::now();

	if ( dispatchSuccessful ) {
		frame.state = FrameData::State::eDispatched;
		//		std::cout << "DISP FRAME " << frameIndex << std::endl
		//		          << std::flush;

	} else {

		std::cout << "NOTICE: Present failed on frame: " << std::dec << frame.frameNumber << std::endl
		          << std::flush;

		// Present was not successful -
		//
		// This most likely happened because the window surface has been resized.
		// We therefore attempt to reset the swapchain.

		frame.state = FrameData::State::eFailedDispatch;

		self->swapchainDirty = true;
	}
}

static void render_tasks( le_renderer_o *renderer, size_t frameIndex ) {

	//	std::cout << "RENDER FRAME " << frameIndex << std::endl
	//	          << std::flush;

	// acquire external backend resources such as swapchain
	// and create any temporary resources
	renderer_acquire_backend_resources( renderer, frameIndex );

	// generate api commands for the frame
	renderer_process_frame( renderer, frameIndex );

	renderer_dispatch_frame( renderer, frameIndex );
}

static void clear_task( le_renderer_o *renderer, size_t frameIndex ) {
	renderer_clear_frame( renderer, frameIndex );
}

struct RenderTask : public enki::ITaskSet {
	size_t         frameIndex;
	le_renderer_o *renderer;
	virtual void   ExecuteRange( enki::TaskSetPartition range, uint32_t threadnum ) override {
		render_tasks( renderer, frameIndex );
	}
	virtual ~RenderTask() = default;
};

struct RecordTask : public enki::ITaskSet {
	size_t              frameIndex;
	le_renderer_o *     renderer;
	le_render_module_o *module;
	virtual void        ExecuteRange( enki::TaskSetPartition range, uint32_t threadnum ) override {
	}
	virtual ~RecordTask() = default;
};

struct ClearTask : public enki::ITaskSet {
	size_t         frameIndex;
	le_renderer_o *renderer;
	virtual void   ExecuteRange( enki::TaskSetPartition range, uint32_t threadnum ) override {
		clear_task( renderer, frameIndex );
	}
	virtual ~ClearTask() = default;
};

// ----------------------------------------------------------------------

static le_resource_handle_t renderer_get_swapchain_resource( le_renderer_o *self ) {
	using namespace le_backend_vk; // for rendergraph_i
	return vk_backend_i.get_swapchain_resource( self->backend );
}

// ----------------------------------------------------------------------

static void renderer_get_swapchain_extent( le_renderer_o *self, uint32_t *p_width, uint32_t *p_height ) {
	using namespace le_backend_vk; // for swapchain
	vk_backend_i.get_swapchain_extent( self->backend, p_width, p_height );
}

// ----------------------------------------------------------------------

static void renderer_update( le_renderer_o *self, le_render_module_o *module_ ) {

	using namespace le_backend_vk; // for vk_backend_i

	const auto &index     = self->currentFrameNumber;
	const auto &numFrames = self->frames.size();

	// If necessary, recompile and reload shader modules
	// - this must be complete before the record_frame step

	vk_backend_i.update_shader_modules( self->backend );

	if ( LE_RENDERER_MULTITHREADED ) {
		// use task system (experimental)

		//		std::cout << "RENDERER UPDATE" << std::endl
		//		          << std::endl
		//		          << std::flush;

		ClearTask clearTask;
		clearTask.renderer   = self;
		clearTask.frameIndex = ( index + 1 ) % numFrames;
		self->g_TS.AddTaskSetToPipe( &clearTask );

		RenderTask renderTask;
		renderTask.renderer   = self;
		renderTask.frameIndex = ( index + 2 ) % numFrames;
		self->g_TS.AddTaskSetToPipe( &renderTask );

		// we record on the main thread.
		renderer_record_frame( self, ( index + 0 ) % numFrames, module_, self->currentFrameNumber ); // generate an intermediary, api-agnostic, representation of the frame

		self->g_TS.WaitforTaskSet( &renderTask );
		self->g_TS.WaitforTaskSet( &clearTask );

	} else {

		// render on the main thread

		renderer_record_frame( self, ( index + 0 ) % numFrames, module_, self->currentFrameNumber ); // generate an intermediary, api-agnostic, representation of the frame
		render_tasks( self, ( index + 2 ) % numFrames );
		renderer_clear_frame( self, ( index + 1 ) % numFrames ); // wait for frame to come back (important to do this last, as it may block...)
	}

	if ( self->swapchainDirty ) {
		// we must dispatch, then clear all previous dispatchable frames,
		// before recreating swapchain. This is because this frame
		// was processed against the vkImage object from the previous
		// swapchain.

		// TODO: check if you could just signal these fences so that the
		// leftover frames must not be dispatched.

		for ( size_t i = 0; i != self->frames.size(); ++i ) {
			if ( self->frames[ i ].state == FrameData::State::eProcessed ) {
				renderer_dispatch_frame( self, i );
				renderer_clear_frame( self, i );
			} else if ( self->frames[ i ].state != FrameData::State::eDispatched ) {
				renderer_clear_frame( self, i );
			}
		}

		vk_backend_i.reset_swapchain( self->backend );

		self->swapchainDirty = false;
	}

	++self->currentFrameNumber;
}

// ----------------------------------------------------------------------

static le_resource_info_t get_default_resource_info_for_image() {
	le_resource_info_t res;

	res.type = LeResourceType::eImage;
	{
		auto &img         = res.image;
		img.flags         = 0;
		img.format        = le::Format::eUndefined;
		img.arrayLayers   = 1;
		img.extent.width  = 0;
		img.extent.height = 0;
		img.extent.depth  = 1;
		img.usage         = {LE_IMAGE_USAGE_SAMPLED_BIT};
		img.mipLevels     = 1;
		img.samples       = le::SampleCountFlagBits::e1;
		img.imageType     = le::ImageType::e2D;
		img.tiling        = le::ImageTiling::eOptimal;
	}

	return res;
}

// ----------------------------------------------------------------------

static le_resource_info_t get_default_resource_info_for_buffer() {
	le_resource_info_t res;
	res.type         = LeResourceType::eBuffer;
	res.buffer.size  = 0;
	res.buffer.usage = {LE_BUFFER_USAGE_TRANSFER_DST_BIT};
	return res;
}

// ----------------------------------------------------------------------

ISL_API_ATTR void register_le_renderer_api( void *api_ ) {
	auto  le_renderer_api_i = static_cast<le_renderer_api *>( api_ );
	auto &le_renderer_i     = le_renderer_api_i->le_renderer_i;

	le_renderer_i.create                 = renderer_create;
	le_renderer_i.destroy                = renderer_destroy;
	le_renderer_i.setup                  = renderer_setup;
	le_renderer_i.update                 = renderer_update;
	le_renderer_i.create_shader_module   = renderer_create_shader_module;
	le_renderer_i.get_swapchain_resource = renderer_get_swapchain_resource;
	le_renderer_i.get_swapchain_extent   = renderer_get_swapchain_extent;
	le_renderer_i.get_pipeline_manager   = renderer_get_pipeline_manager;
	le_renderer_i.get_backend            = renderer_get_backend;

	auto &helpers_i = le_renderer_api_i->helpers_i;

	helpers_i.get_default_resource_info_for_buffer = get_default_resource_info_for_buffer;
	helpers_i.get_default_resource_info_for_image  = get_default_resource_info_for_image;

	// register sub-components of this api
	register_le_rendergraph_api( api_ );

	register_le_command_buffer_encoder_api( api_ );
}