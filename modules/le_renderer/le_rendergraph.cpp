#include "le_renderer.h"

#include "le_backend_vk.h"

#include <cstring>
#include <vector>
#include <string>
#include <assert.h>
#include <algorithm>
#include <unordered_map>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <sstream>
#include <array>

#include "private/le_resource_handle_t.inl"

#include "le_log.h"

static constexpr auto LOGGER_LABEL = "le_rendergraph";

#ifdef _MSC_VER
#	include <Windows.h>
#else
#	include <unistd.h> // for getexepath
#endif

#include "3rdparty/src/spooky/SpookyV2.h" // for calculating rendergraph hash

#ifndef LE_PRINT_DEBUG_MESSAGES
#	define LE_PRINT_DEBUG_MESSAGES false
#endif

#ifndef LE_GENERATE_DOT_GRAPH
#	ifndef NDEBUG
#		define LE_GENERATE_DOT_GRAPH true
#	else
#		define LE_GENERATE_DOT_GRAPH false
#	endif
#endif

#include <bitset>

using ResourceField = std::bitset<LE_MAX_NUM_GRAPH_RESOURCES>; // Each bit represents a distinct resource
// ----------------------------------------------------------------------

namespace le {
using RWFlags = uint32_t;
enum class ResourceAccessFlagBits : RWFlags {
	eUndefined = 0x0,
	eRead      = 0x1 << 0,
	eWrite     = 0x1 << 1,
	eReadWrite = eRead | eWrite,
};

constexpr RWFlags operator|( ResourceAccessFlagBits const& lhs, ResourceAccessFlagBits const& rhs ) noexcept {
	return static_cast<const RWFlags>( static_cast<RWFlags>( lhs ) | static_cast<RWFlags>( rhs ) );
};

constexpr RWFlags operator|( RWFlags const& lhs, ResourceAccessFlagBits const& rhs ) noexcept {
	return static_cast<const RWFlags>( lhs | static_cast<RWFlags>( rhs ) );
};

constexpr RWFlags operator&( ResourceAccessFlagBits const& lhs, ResourceAccessFlagBits const& rhs ) noexcept {
	return static_cast<const RWFlags>( static_cast<RWFlags>( lhs ) & static_cast<RWFlags>( rhs ) );
};
} // namespace le

// ----------------------------------------------------------------------
static constexpr le::AccessFlags2 LE_ALL_READ_ACCESS_FLAGS =
    le::AccessFlagBits2::eIndirectCommandRead |
    le::AccessFlagBits2::eIndexRead |
    le::AccessFlagBits2::eVertexAttributeRead |
    le::AccessFlagBits2::eUniformRead |
    le::AccessFlagBits2::eInputAttachmentRead |
    le::AccessFlagBits2::eShaderRead |
    le::AccessFlagBits2::eColorAttachmentRead |
    le::AccessFlagBits2::eDepthStencilAttachmentRead |
    le::AccessFlagBits2::eTransferRead |
    le::AccessFlagBits2::eHostRead |
    le::AccessFlagBits2::eMemoryRead |
    le::AccessFlagBits2::eCommandPreprocessReadBitNv |
    le::AccessFlagBits2::eColorAttachmentReadNoncoherentBitExt |
    le::AccessFlagBits2::eConditionalRenderingReadBitExt |
    le::AccessFlagBits2::eAccelerationStructureReadBitKhr |
    le::AccessFlagBits2::eTransformFeedbackCounterReadBitExt |
    le::AccessFlagBits2::eFragmentDensityMapReadBitExt |
    le::AccessFlagBits2::eFragmentShadingRateAttachmentReadBitKhr |
    le::AccessFlagBits2::eShaderSampledRead |
    le::AccessFlagBits2::eShaderStorageRead |
    le::AccessFlagBits2::eVideoDecodeReadBitKhr |
    le::AccessFlagBits2::eVideoEncodeReadBitKhr |
    le::AccessFlagBits2::eInvocationMaskReadBitHuawei;

static constexpr le::AccessFlags2 LE_ALL_WRITE_ACCESS_FLAGS =
    le::AccessFlagBits2::eShaderWrite |
    le::AccessFlagBits2::eColorAttachmentWrite |
    le::AccessFlagBits2::eDepthStencilAttachmentWrite |
    le::AccessFlagBits2::eTransferWrite |
    le::AccessFlagBits2::eHostWrite |
    le::AccessFlagBits2::eMemoryWrite |
    le::AccessFlagBits2::eCommandPreprocessWriteBitNv |
    le::AccessFlagBits2::eAccelerationStructureWriteBitKhr |
    le::AccessFlagBits2::eTransformFeedbackWriteBitExt |
    le::AccessFlagBits2::eTransformFeedbackCounterWriteBitExt |
    le::AccessFlagBits2::eVideoDecodeWriteBitKhr |
    le::AccessFlagBits2::eVideoEncodeWriteBitKhr |
    le::AccessFlagBits2::eShaderStorageWrite //
    ;
static constexpr le::AccessFlags2 LE_ALL_IMAGE_IMPLIED_WRITE_ACCESS_FLAGS =
    le::AccessFlagBits2::eShaderSampledRead | //
    le::AccessFlagBits2::eShaderRead |        // shader read is a potential read/write operation, as it might imply a layout transform
    le::AccessFlagBits2::eShaderStorageRead   // this might mean a read/write in case we are accessing an image as it might imply a layout transform
    ;

// ----------------------------------------------------------------------

static std::string to_string_le_access_flags2( const le::AccessFlags2& tp ) {
	uint64_t    flags = tp;
	std::string result;
	int         bit_pos = 0;
	while ( flags ) {
		if ( flags & 1 ) {
			if ( false == result.empty() ) {
				result.append( " | " );
			}
			result.append( to_str( le::AccessFlagBits2( 1ULL << bit_pos ) ) );
		}
		flags >>= 1;
		bit_pos++;
	}
	return result;
}

// ----------------------------------------------------------------------

struct Node {
	ResourceField       reads               = 0;
	ResourceField       writes              = 0;
	le::RootPassesField root_index_affinity = 0;     // association of node with root node(s) - each bit represents a root node, if set, this pass contributes to that particular root node
	bool                is_root             = false; // whether this node is a root node
	bool                is_contributing     = false; // whether this node contributes to a root node
};

// these are some sanity checks for le_renderer_types
static_assert( sizeof( le::CommandHeader ) == sizeof( uint64_t ), "Size of le::CommandHeader must be 64bit" );

struct ExecuteCallbackInfo {
	le_renderer_api::pfn_renderpass_execute_t fn        = nullptr;
	void*                                     user_data = nullptr;
};

struct le_renderpass_o {

	le::QueueFlagBits       type         = le::QueueFlagBits{};         // Requirements for a queue to which this pass can be submitted.
	uint32_t                ref_count    = 0;                           // reference count (we're following an intrusive shared pointer pattern)
	uint64_t                id           = 0;                           // hash of name
	uint32_t                width        = 0;                           // < width  in pixels, must be identical for all attachments, default:0 means current frame.swapchainWidth
	uint32_t                height       = 0;                           // < height in pixels, must be identical for all attachments, default:0 means current frame.swapchainHeight
	le::SampleCountFlagBits sample_count = le::SampleCountFlagBits::e1; // < SampleCount for all attachments.

	uint32_t            is_root = false;      // Whether pass *must* be processed
	le::RootPassesField root_passes_affinity; // Association of this renderpass with one or more root passes that it contributes to -
	                                          // this needs to be communicated to backend, so that you may create queue submissions
	                                          // by filtering via root_passes_affinity_masks

	std::vector<le_resource_handle> resources;                  // all resources used in this pass, contains info about resource type
	std::vector<le::RWFlags>        resources_read_write_flags; // TODO: get rid of this: we can use resources_access_flags instead. read/write flags for all resources, in sync with resources
	std::vector<le::AccessFlags2>   resources_access_flags;     // first read | last write access for each resource used in this pass

	std::vector<le_image_attachment_info_t> imageAttachments;    // settings for image attachments (may be color/or depth)
	std::vector<le_img_resource_handle>     attachmentResources; // kept in sync with imageAttachments, one resource per attachment

	std::vector<le_texture_handle>       textureIds;   // imageSampler resource infos
	std::vector<le_image_sampler_info_t> textureInfos; // kept in sync with texture id: info for corresponding texture id

	le_renderer_api::pfn_renderpass_setup_t callbackSetup            = nullptr;
	void*                                   setup_callback_user_data = nullptr;
	std::vector<ExecuteCallbackInfo>        executeCallbacks;

	le_command_buffer_encoder_o* encoder = nullptr;
	char                         debugName[ 256 ];
};

// ----------------------------------------------------------------------

struct le_rendergraph_o : NoCopy, NoMove {
	std::vector<le_renderpass_o*>    passes;                     //
	std::vector<le_resource_handle>  declared_resources_id;      // | pre-declared resources (declared via module)
	std::vector<le_resource_info_t>  declared_resources_info;    // | pre-declared resources (declared via module)
	std::vector<le::RootPassesField> root_passes_affinity_masks; // vector of masks, one per distinct tree within the rendergraph,
	                                                             // each mask represents a filter: passes whose root_passes_affinity
	                                                             // match via OR are contributing to the distinct tree whose key it was tested against.
	                                                             // Each entry represents a distinct tree which can be submitted as a
	                                                             // separate (and resource-isolated) queue submission.
};

// ----------------------------------------------------------------------

static le_renderpass_o* renderpass_create( const char* renderpass_name, const le::QueueFlagBits& type_ ) {
	auto self  = new le_renderpass_o();
	self->id   = hash_64_fnv1a( renderpass_name );
	self->type = type_;
	strncpy( self->debugName, renderpass_name, sizeof( self->debugName ) );
	self->ref_count = 1;
	return self;
}

// ----------------------------------------------------------------------

static le_renderpass_o* renderpass_clone( le_renderpass_o const* rhs ) {
	auto self       = new le_renderpass_o();
	*self           = *rhs;
	self->ref_count = 1;
	return self;
}

// ----------------------------------------------------------------------

static void renderpass_destroy( le_renderpass_o* self ) {

	if ( self->encoder ) {
		using namespace le_renderer;
		encoder_i.destroy( self->encoder );
	}

	delete self;
}

static void renderpass_ref_inc( le_renderpass_o* self ) {
	++self->ref_count;
}

static void renderpass_ref_dec( le_renderpass_o* self ) {
	if ( --self->ref_count == 0 ) {
		renderpass_destroy( self );
	}
}

// ----------------------------------------------------------------------

static void renderpass_set_setup_callback( le_renderpass_o* self, void* user_data, le_renderer_api::pfn_renderpass_setup_t callback ) {
	self->setup_callback_user_data = user_data;
	self->callbackSetup            = callback;
}

// ----------------------------------------------------------------------

static void renderpass_set_execute_callback( le_renderpass_o* self, void* user_data, le_renderer_api::pfn_renderpass_execute_t callback ) {
	self->executeCallbacks.push_back( { callback, user_data } );
}

// ----------------------------------------------------------------------
static void renderpass_run_execute_callbacks( le_renderpass_o* self ) {
	for ( auto const& c : self->executeCallbacks ) {
		c.fn( self->encoder, c.user_data );
	}
}

// ----------------------------------------------------------------------
static bool renderpass_run_setup_callback( le_renderpass_o* self ) {
	return self->callbackSetup( self, self->setup_callback_user_data );
}

// ----------------------------------------------------------------------
template <typename T>
static inline bool vector_contains( const std::vector<T>& haystack, const T& needle ) noexcept {
	return haystack.end() != std::find( haystack.begin(), haystack.end(), needle );
}

static inline bool resource_is_a_swapchain_handle( const le_img_resource_handle& handle ) {
	return handle->data->flags == le_img_resource_usage_flags_t::eIsRoot;
}

// ----------------------------------------------------------------------
// Associate a resource with a renderpass.
// Data containted in `resource_info` decides whether the resource
// is used for read, write, or read/write.
static void renderpass_use_resource( le_renderpass_o* self, const le_resource_handle& resource_id, le::AccessFlags2 const& access_flags ) {

	static auto logger = LeLog( LOGGER_LABEL );

	size_t resource_idx    = 0; // index of matching resource
	size_t resources_count = self->resources.size();
	for ( le_resource_handle* res = self->resources.data(); resource_idx != resources_count; res++ ) {
		if ( *res == resource_id ) {
			// found a match
			break;
		}
		resource_idx++;
	}

	if ( resource_idx == resources_count ) {
		// not found, add resource and resource info
		self->resources.push_back( resource_id );
		// Note that we don't immediately set the access flag,
		// as the correct access flag is calculated based on resource_info
		// after this block.
		self->resources_read_write_flags.push_back( le::RWFlags( le::ResourceAccessFlagBits::eUndefined ) );
		self->resources_access_flags.push_back( access_flags );

	} else {

		// Resource was already used : this should be fine if declared with identical access_flags,
		// otherwise it is an error.
		assert( false );
		self->resources_access_flags[ resource_idx ] = self->resources_access_flags[ resource_idx ] | access_flags;
	}

	//	le::Log( LOGGER_LABEL ).info( "pass: [ %20s ] use resource: %40s, access { %-60s }", self->debugName, resource_id->data->debug_name, to_string_le_access_flags2( access_flags ).c_str() );

	bool detectRead  = ( access_flags & LE_ALL_READ_ACCESS_FLAGS );
	bool detectWrite = ( access_flags & LE_ALL_WRITE_ACCESS_FLAGS );

	// In case we have an IMAGE resource, we might have to do an image layout transform, which is a read/write operation -
	// this means that some reads to image resources are implicit read/writes.
	// we can only get rid of this if we can prove that resources will not undergo a layout transform.
	//
	if ( resource_id->data->type == LeResourceType::eImage ) {
		detectWrite |= ( access_flags & LE_ALL_IMAGE_IMPLIED_WRITE_ACCESS_FLAGS );
	}

	// update access flags
	le::RWFlags& rw_flags = self->resources_read_write_flags[ resource_idx ];

	if ( detectRead ) {
		rw_flags = rw_flags | le::ResourceAccessFlagBits::eRead;
	}

	if ( detectWrite ) {

		if ( resource_id->data->type == LeResourceType::eImage &&
		     resource_is_a_swapchain_handle( static_cast<le_img_resource_handle>( resource_id ) ) ) {
			// A request to write to swapchain image automatically turns a pass into a root pass.
			self->is_root = true;
		}

		rw_flags = rw_flags | le::ResourceAccessFlagBits::eWrite;
	}
}

// ----------------------------------------------------------------------
static void renderpass_sample_texture( le_renderpass_o* self, le_texture_handle texture, le_image_sampler_info_t const* textureInfo ) {

	// -- store texture info so that backend can create resources

	if ( vector_contains( self->textureIds, texture ) ) {
		return; // texture already present
	}

	// --------| invariant: texture id was not previously known

	// -- Add texture info to list of texture infos for this frame
	self->textureIds.push_back( texture );
	//	self->textureImageIds.push_back( textureInfo->imageView.imageId );
	self->textureInfos.push_back( *textureInfo ); // store a copy of info

	le::AccessFlags2 access_flags = le::AccessFlags2( le::AccessFlagBits2::eShaderSampledRead );
	// -- Mark image resource referenced by texture as used for reading
	renderpass_use_resource( self, textureInfo->imageView.imageId, access_flags );
}

// ----------------------------------------------------------------------

static void renderpass_add_color_attachment( le_renderpass_o* self, le_img_resource_handle image_id, le_image_attachment_info_t const* attachmentInfo ) {

	self->imageAttachments.push_back( *attachmentInfo );
	self->attachmentResources.push_back( image_id );

	le::AccessFlags2 access_flags{};

	if ( attachmentInfo->loadOp == le::AttachmentLoadOp::eLoad ) {
		access_flags = access_flags | le::AccessFlags2( le::AccessFlagBits2::eColorAttachmentRead );
	}
	if ( attachmentInfo->storeOp == le::AttachmentStoreOp::eStore ) {
		access_flags = access_flags | le::AccessFlags2( le::AccessFlagBits2::eColorAttachmentWrite );
	}

	renderpass_use_resource( self, image_id, access_flags );
}

// ----------------------------------------------------------------------

static void renderpass_add_depth_stencil_attachment( le_renderpass_o* self, le_img_resource_handle image_id, le_image_attachment_info_t const* attachmentInfo ) {

	self->imageAttachments.push_back( *attachmentInfo );
	self->attachmentResources.push_back( image_id );

	le::AccessFlags2 access_flags{};

	if ( attachmentInfo->loadOp == le::AttachmentLoadOp::eLoad ) {
		access_flags = access_flags | le::AccessFlags2( le::AccessFlagBits2::eDepthStencilAttachmentRead );
	}
	if ( attachmentInfo->storeOp == le::AttachmentStoreOp::eStore ) {
		access_flags = access_flags | le::AccessFlags2( le::AccessFlagBits2::eDepthStencilAttachmentWrite );
	}
	renderpass_use_resource( self, image_id, access_flags );
}

// ----------------------------------------------------------------------

static bool renderpass_get_framebuffer_settings( le_renderpass_o const* self, uint32_t* width, uint32_t* height, le::SampleCountFlagBits* sample_count ) {
	if ( self->type != le::QueueFlagBits::eGraphics ) {
		return false; // only graphics passes have width, height, and sample_count
	}
	if ( width ) {
		*width = self->width;
	}
	if ( height ) {
		*height = self->height;
	}
	if ( sample_count ) {
		*sample_count = self->sample_count;
	}
	// only return true if the current pass is graphics pass
	return true;
}

static void renderpass_set_width( le_renderpass_o* self, uint32_t width ) {
	self->width = width;
}

static void renderpass_set_height( le_renderpass_o* self, uint32_t height ) {
	self->height = height;
}

static void renderpass_set_sample_count( le_renderpass_o* self, le::SampleCountFlagBits const& sampleCount ) {
	self->sample_count = sampleCount;
}

// ----------------------------------------------------------------------

static void renderpass_set_is_root( le_renderpass_o* self, bool is_root ) {
	self->is_root = is_root;
}

static bool renderpass_get_is_root( le_renderpass_o const* self ) {
	return self->is_root;
}

static void renderpass_get_queue_submission_info( const le_renderpass_o* self, le::QueueFlagBits* pass_type, le::RootPassesField* queue_submission_id ) {
	if ( pass_type ) {
		*pass_type =
		    self->type;
	}
	if ( queue_submission_id ) {
		*queue_submission_id = self->root_passes_affinity;
	}
}

static void renderpass_get_used_resources( le_renderpass_o const* self, le_resource_handle const** pResources, le::AccessFlags2 const** pResourcesAccess, size_t* count ) {
	assert( self->resources_access_flags.size() == self->resources.size() );

	*count            = self->resources.size();
	*pResources       = self->resources.data();
	*pResourcesAccess = self->resources_access_flags.data();
}

static const char* renderpass_get_debug_name( le_renderpass_o const* self ) {
	return self->debugName;
}

static uint64_t renderpass_get_id( le_renderpass_o const* self ) {
	return self->id;
}

static void renderpass_get_image_attachments( const le_renderpass_o* self, le_image_attachment_info_t const** pAttachments,
                                              le_img_resource_handle const** pResources, size_t* numAttachments ) {
	*pAttachments   = self->imageAttachments.data();
	*pResources     = self->attachmentResources.data();
	*numAttachments = self->imageAttachments.size();
}

static void renderpass_get_texture_ids( le_renderpass_o* self, le_texture_handle const** ids, uint64_t* count ) {
	*ids   = self->textureIds.data();
	*count = self->textureIds.size();
};

static void renderpass_get_texture_infos( le_renderpass_o* self, const le_image_sampler_info_t** infos, uint64_t* count ) {
	*infos = self->textureInfos.data();
	*count = self->textureInfos.size();
};

static bool renderpass_has_execute_callback( const le_renderpass_o* self ) {
	return !self->executeCallbacks.empty();
}

static bool renderpass_has_setup_callback( const le_renderpass_o* self ) {
	return self->callbackSetup != nullptr;
}

/// @warning Encoder becomes the thief's worry to destroy!
/// @returns null if encoder was already stolen, otherwise a pointer to an encoder object
le_command_buffer_encoder_o* renderpass_steal_encoder( le_renderpass_o* self ) {
	auto result   = self->encoder;
	self->encoder = nullptr;
	return result;
}

// ----------------------------------------------------------------------

static le_rendergraph_o* rendergraph_create() {
	auto obj = new le_rendergraph_o();
	return obj;
}

// ----------------------------------------------------------------------

static void rendergraph_reset( le_rendergraph_o* self ) {

	// we must destroy passes as we have ownership over them.
	for ( auto rp : self->passes ) {
		renderpass_destroy( rp );
	}
	self->passes.clear();

	self->root_passes_affinity_masks.clear();
	self->declared_resources_id.clear();
	self->declared_resources_info.clear();
}

// ----------------------------------------------------------------------

static void rendergraph_destroy( le_rendergraph_o* self ) {
	rendergraph_reset( self );
	delete self;
}

// ----------------------------------------------------------------------

static void rendergraph_add_renderpass( le_rendergraph_o* self, le_renderpass_o* renderpass ) {
	self->passes.push_back( renderpass_clone( renderpass ) ); // Note: We receive ownership of the pass here. We must destroy it.
}

/// \brief Tag any nodes which contribute to any root nodes
/// \details We do this so that we can weed out any nodes which are provably
///          not contributing - these don't need to be executed at all.
static void node_tag_contributing( Node* const nodes, const size_t num_nodes, uint32_t* count_roots = nullptr ) {

	// we must iterate backwards from last layer to first layer
	Node*             node      = nodes + num_nodes;
	Node const* const node_rend = nodes;

	ResourceField read_accum;

	if ( count_roots ) {
		*count_roots = 0;
	}
	// find first root layer
	//    monitored reads will be from the first root layer

	// TODO: for each root node, we want to accumulate QueueFlagBits from
	// nodes that contribute.

	while ( node != node_rend ) {
		--node;

		// If it's a root node, get all reads from (= providers to) this node
		//      if node is tagged being a root node and it writes to any monitored read, it can't be a root node.
		// If it's not a root node, first see if there are any writes to currently monitored reads
		//      if yes, add all reads to monitored reads

		bool writes_to_any_monitored_read = ( node->writes & read_accum ).any();

		if ( node->is_root || writes_to_any_monitored_read ) {

			// If this node is a root node - OR					      ) this means the layer is contributing
			// If this node writes to any subsequent monitored reads, )
			// Then we must monitor all reads by this node.
			//
			// If a write has no corresponding read (we see a write-only operation), it will extinguish
			// the read bit at this place - this makes sense, as any previous writes to this place will
			// be implicitly discarded by a write-only operation onto this place. (Any previous writes
			// are never read, and we will need a new read to make this resource active again)

			read_accum = ( read_accum & ( ~node->writes ) ) // Anything written in this node will be extinguished (consumed)
			             | node->reads;                     // Anything read in this node will be lit up.

			node->is_contributing = true;

			if ( node->is_root && writes_to_any_monitored_read ) {
				// Node cannot be root if it writes to a monitored read (this means that another more-root node depends on it)
				node->is_root = false;
			}
			if ( node->is_root && count_roots ) {
				( *count_roots )++;
			}
		}

	} // end for all nodes, backwards iteration
}

// ----------------------------------------------------------------------
// Generates a .dot file for graphviz which visualises renderpasses
// and their resource dependencies. It will also show the sequencing
// of how renderpasses are executed, beginning at the top.
//
// The graphviz file is stored as graph.dot in the executable's directory.
//
static bool generate_dot_file_for_rendergraph(
    le_rendergraph_o*   self,
    le_resource_handle* uniqueResources,
    size_t const&       numUniqueResources,
    Node const*         nodes,
    size_t              frame_number ) {

	static auto                  logger   = LeLog( LOGGER_LABEL );
	static std::filesystem::path exe_path = []() {
		char result[ 1024 ] = { 0 };

#ifdef _MSC_VER

		// When NULL is passed to GetModuleHandle, the handle of the exe itself is returned
		HMODULE hModule = GetModuleHandle( NULL );
		if ( hModule != NULL ) {
			// Use GetModuleFileName() with module handle to get the path
			GetModuleFileName( hModule, result, ( sizeof( result ) ) );
		}
		size_t count = strnlen_s( result, sizeof( result ) );
#else
		ssize_t count = readlink( "/proc/self/exe", result, 1024 );
#endif

		return std::string( result, ( count > 0 ) ? size_t( count ) : 0 );
	}();

	std::ostringstream os;

	os << "digraph g {" << std::endl;

	os << "node [shape = plain,height=1,fontname=\"IBM Plex Sans\"];" << std::endl;
	os << "graph [label=<"
	   << "<table border='0' cellborder='0' cellspacing='0' cellpadding='3'>"
	   << "<tr><td align='left'>Island Rendergraph</td></tr>"
	   << "<tr><td align='left'>" << exe_path << "</td></tr>"
	   << "<tr><td align='left'>Frame № " << frame_number << "</td></tr>"
	   << "</table>"
	   << ">"
	   << ", splines=true, nodesep=0.7, fontname=\"IBM Plex Sans\", fontsize=10, labeljust=\"l\"];" << std::endl;

	for ( size_t i = 0; i != self->passes.size(); ++i ) {
		auto const& p = self->passes[ i ];

		if ( nodes[ i ].is_contributing ) {
			os << "\"" << p->debugName << "\""
			   << "[label = <<table border='0' cellborder='1' cellspacing='0'><tr><td border='"
			   << ( nodes[ i ].is_root ? "10" : "0" )
			   << "' sides='b' cellpadding='3'><b>"
			   << ( nodes[ i ].is_root ? "⊥ " : "" )
			   << p->debugName << "</b></td>";
		} else {
			os << "\"" << p->debugName << "\""
			   << "[label = <<table bgcolor='gray' border='0' cellborder='1' cellspacing='0'><tr><td border='"
			   << ( nodes[ i ].is_root ? "10" : "0" )
			   << "' sides='b' cellpadding='3'><b>"
			   << ( nodes[ i ].is_root ? "⊥ " : "" )
			   << p->debugName << "</b></td>";
		}

		if ( p->resources.empty() ) {
			os << "</tr></table>>];" << std::endl;
			continue;
		}

		for ( size_t j = 0; j != p->resources.size(); j++ ) {
			os << "<td cellpadding='3' port=\"";
			auto const& r = p->resources[ j ];
			os << r->data->debug_name << "\">";

			{
				auto const needle = r;

				size_t res_idx = 0; // unique resource id (monotonic, non-sparse, index into bitfield)
				for ( auto ur = uniqueResources; res_idx != numUniqueResources; res_idx++, ur++ ) {
					if ( *ur == needle ) {
						// found matching resource, res_idx is index into uniqueHandles for resource
						break;
					}
				}

				// if resource is being written to, then underline resource name
				if ( nodes[ i ].reads[ res_idx ] ) {
					os << "△";
				}
				if ( nodes[ i ].writes[ res_idx ] ) {
					os << "▼";
				}

				if ( nodes[ i ].writes[ res_idx ] ) {
					os << "<u>" << r->data->debug_name << "</u>";
				} else {
					os << " " << r->data->debug_name << "";
				}
			}

			os << "</td>";
		}
		os << "</tr></table>>];" << std::endl;
	}

	//	// Indicate which passes are of the same rank,
	//	// which we do by grouping passes by their sort order.

	//	{
	//		// we need to group elements with the same sort indices.

	//		// -- get a set of sort indices
	//		// -- for each sort index, list passes with this sort index

	//		std::set<uint32_t> unique_sort_indices;

	//		for ( auto& i : self->sortIndices ) {
	//			unique_sort_indices.insert( i );
	//		}

	//		for ( auto const& i : unique_sort_indices ) {
	//			if ( i == ( ~0u ) ) {
	//				continue;
	//			}
	//			os << "{rank=same; ";
	//			for ( size_t j = 0; j != self->sortIndices.size(); j++ ) {
	//				if ( i == self->sortIndices[ j ] ) {
	//					os << "\"" << self->passes[ j ]->debugName << "\" ";
	//				}
	//			}
	//			os << "}" << std::endl;
	//		}
	//	}

	// Draw connections : A connection goes from each resource that
	// has been written in a pass to all subsequent passes which read
	// from this resource, until a pass writes to the resource again.

	for ( size_t i = 0; i != self->passes.size(); ++i ) {
		auto const& p = self->passes[ i ];

		// For each resource referenced by this pass: find passes which read from it subsequently, until
		// the first pass writes to it again.

		for ( size_t j = 0; j != p->resources.size(); j++ ) {
			// find resources the current pass writes to.

			auto const needle = p->resources[ j ];

			size_t res_idx = 0; // unique resource id (monotonic, non-sparse, index into bitfield)
			for ( auto r = uniqueResources; res_idx != numUniqueResources; res_idx++, r++ ) {
				if ( *r == needle ) {
					// found matching resource, res_idx is index into uniqueHandles for resource
					break;
				}
			}

			assert( res_idx != numUniqueResources && "something went wrong, handle could not be found in list of unique handles." );

			if ( !nodes[ i ].writes[ res_idx ] ) {
				continue;
			}

			// now we must find any subsequent nodes which read from this resource.

			ResourceField res_filter = uint64_t( 1 ) << res_idx;

			for ( size_t k = i + 1; k != self->passes.size(); k++ ) {
				if ( ( nodes[ k ].reads & res_filter ).any() ||
				     ( nodes[ k ].writes & nodes[ k ].reads & res_filter ).any() ) {

					os << "\"" << p->debugName << "\":"
					   << "\"" << needle->data->debug_name << "\""
					   << ":s"
					   << " -> \"" << self->passes[ k ]->debugName << "\":"
					   << "\"" << needle->data->debug_name << "\""
					   << ":n"
					   << ( nodes[ k ].is_contributing == false ? "[style=dashed]" : "" )
					   << ";" << std::endl;
				}
				if ( ( nodes[ k ].writes & res_filter ).any() ) {
					break;
				}
			}
		}
	}

	// for each resource in each pass, if it is a write resource:
	// find the same resource in any subsequent passes which read from it,
	// and create a write dependency
	// until is it written to again.

	os << "}" << std::endl;

	auto write_to_file = []( char const* filename, std::ostringstream const& os ) {
		FILE* out_file = fopen( filename, "wb" );
		fprintf( out_file, "%s\n", os.str().c_str() );
		fclose( out_file );

		logger.info( "Generated .dot file: '%s'", filename );
	};

	// We write to two files: "graph.dot",
	// and then we write the same contents into a file with the frame number in the
	// filename so that we may keep a history of rendergraphs...
	char filename[ 32 ] = "graph.dot";

	std::filesystem::path full_path = exe_path.parent_path() / filename;
	write_to_file( full_path.string().c_str(), os );

	snprintf( filename, sizeof( filename ), "graph_%08zu.dot", frame_number );

	full_path = exe_path.parent_path() / filename;
	write_to_file( full_path.string().c_str(), os );

	return true;
};

extern le_resource_handle renderer_produce_resource_handle(
    char const*           maybe_name,
    LeResourceType const& resource_type,
    uint8_t               num_samples      = 0,
    uint8_t               flags            = 0,
    uint16_t              index            = 0,
    le_resource_handle    reference_handle = nullptr );
// ----------------------------------------------------------------------
// Calculate a topological order for passes within rendergraph.
//
// We assume that passes arrive in partial-order (i.e. the order
// of adding passes to a module is meaningful)
//
// As a side-effect, this method:
// + Removes (and deletes) any passes which do not contribute from a rendergraph.
// + Updates sortIndices so that it has same number of elements as rendergraph.
// After completion this method guarantees that sortIndices constains a valid
// sort index for each corresponding renderpass.
//
static void rendergraph_build( le_rendergraph_o* self, size_t frame_number ) {

	static auto logger = LeLog( LOGGER_LABEL );

	// We must express our list of passes as a list of nodes.
	// A node holds two bitfields, the bitfield names are: `read` and `write`.
	// Each bit in the bitfield represents a possible resource.
	// This means we must create a list of unique resources, so that we can use the resource index as the
	// offset value for a bit representing this particular resource in the bitfields.

	std::vector<Node>                                          nodes;
	std::array<le_resource_handle, LE_MAX_NUM_GRAPH_RESOURCES> uniqueHandles; // lookup for resource handles.
	size_t                                                     numUniqueResources = 0;

	// Translate all passes into a node
	//   Get list of resources per pass and build node from this

	for ( auto const& p : self->passes ) {

		Node node{};

		const size_t numResources = p->resources.size();

		for ( size_t i = 0; i != numResources; i++ ) {
			auto const&        resource_handle = p->resources[ i ];
			le::RWFlags const& access_flags    = p->resources_read_write_flags[ i ];

			size_t res_idx = 0; // unique resource id (monotonic, non-sparse, index into bitfield)
			for ( auto r = uniqueHandles.data(); res_idx != numUniqueResources; res_idx++, r++ ) {
				if ( *r == resource_handle ) {
					// found matching resource, res_idx is index into uniqueHandles for resource
					break;
				}
			}

			if ( res_idx == numUniqueResources ) {
				// resource was not found, we must add a new resource
				uniqueHandles[ res_idx ] = resource_handle;
				numUniqueResources++;
				assert( numUniqueResources < LE_MAX_NUM_GRAPH_RESOURCES && "bitfield must be large enough to provide one field for each unique resource" );
			}

			// --------| invariant: uniqueHandles[res_idx] is valid

			node.reads.set( res_idx, ( le::ResourceAccessFlagBits( access_flags ) & le::ResourceAccessFlagBits::eRead ) );
			node.writes.set( res_idx, ( ( le::ResourceAccessFlagBits( access_flags ) & le::ResourceAccessFlagBits::eWrite ) >> 1 ) );
		}

		if ( p->is_root ) {
			node.is_root = true;
		}

		nodes.emplace_back( std::move( node ) );
	}

	// Tag all nodes which contribute to any root node.
	//
	// Tasks which don't contribute to any root node
	// can be disposed, as their products will never be used.
	uint32_t root_count = 0; // gets set to number of found root nodes as a side-effect of node_tag_contributing
	node_tag_contributing( nodes.data(), nodes.size(), &root_count );
	assert( root_count <= LE_MAX_NUM_GRAPH_ROOTS && "number of nodes must fit LE_MAX_NUM_TREES, otherwise we can't express tree affinity as a bitfield" );

	{
		std::vector<ResourceField> root_reads_accum( root_count );
		std::vector<ResourceField> root_writes_accum( root_count );

		// for each root node, accumulate all reads, and writes from contributing nodes.
		// we do this so that we can test whether each tree is isolated.

		// a tree is isolated if none of its writes touch any other tree's reads.
		// - what happens if a tree's reads touch another tree's write?
		{

			uint64_t root_index = 0;

			// find first root node by iterating backwards
			for ( auto r = nodes.rbegin(); r != nodes.rend(); r++ ) {
				while ( r != nodes.rend() && !r->is_root ) {
					r++;
				}
				if ( r == nodes.rend() ) {
					break;
				}
				ResourceField& read_accum  = root_reads_accum[ root_index ];
				ResourceField& write_accum = root_writes_accum[ root_index ];
				// n is a root node.
				read_accum  = r->reads;
				write_accum = r->writes;
				r->root_index_affinity |= ( 1ULL << root_index );

				for ( auto n = r + 1; n != nodes.rend(); n++ ) {
					if ( n->is_root ) {
						continue;
					}
					// if this earlier node writes to any of our subsequent reads, we add it to our
					// current tree of nodes.
					if ( ( n->writes & read_accum ).any() ) {
						read_accum |= n->reads;
						write_accum |= n->writes;
						// tag resource as belonging to this particular root node.
						n->root_index_affinity |= ( 1ULL << root_index );
					}
				}
				root_index++;
			}
		}

#if ( LE_PRINT_DEBUG_MESSAGES )
		{
			logger.info( "Unique resources:" );
			for ( size_t i = 0; i != numUniqueResources; i++ ) {
				logger.info( "%3d : %s", i, uniqueHandles[ i ]->data->debug_name );
			}
		}
		for ( size_t i = 0; i < root_count; i++ ) {
			logger.info( "root node (%2d)", i );
			logger.info( "reads : %s", root_reads_accum[ i ].to_string().c_str() );
			logger.info( "writes: %s", root_writes_accum[ i ].to_string().c_str() );
		}

		logger.info( "" );
		for ( size_t i = 0; i < self->passes.size(); i++ ) {
			logger.info( "node %-20s, affinity: %x", self->passes[ i ]->debugName, nodes[ i ].root_index_affinity );
		}
		logger.info( "" );
#endif
		// For each root pass, test its accumulated reads against all other root passes' accumulated writes.
		// if there is an overlap, we know that the two roots which overlap must be combined, as
		// they are not perfectly resource-isolated (an overlap means that one writes on the other's
		// read resource).
		//
		// No such overlap is detected if both roots in a test only read from the same resource -
		// this is because two queues may read concurrently from a resource.
		//
		// We need to test each root against all other roots - note that we can to two tests at once.
		// Meaning our complexity for n root elements is ((n^2-n)/2)
		//
		// Q: What happens when more than two batches overlap?
		// A: Exactly what you would expect, all overlapping batches form one giant combined batch.
		//

		// Initially, each root starts out on their own queue -
		// each queue id is initially a unique single bit in the queue_id bitfield
		//
		// If we detect overlap, roots which overlap will get their queue_ids OR'ed
		// and both roots update the queue_id they point to - so that they point to the new, combined id
		//
		// By the end ot this process we get a list of unique queue_ids which have no overlap.
		//
		std::vector<le::RootPassesField> queue_id( root_count );     // queue id per root - starting out with a single bit
		std::vector<int>                 queue_id_idx( root_count ); // queue id index per root
		for ( size_t i = 0; i != root_count; i++ ) {
			queue_id[ i ] |= ( 1ULL << i ); // initialise to single bit at bitfield position corresponding to queue id
			queue_id_idx[ i ] = i;          // initialise queue id index to be direct mapping
		}

		for ( size_t i = 0; i != root_count; i++ ) {
			for ( size_t j = i + 1; j != root_count; j++ ) {
				// compare i <-> j
				// compare j <-> i
				// If any reads appear in writes, tag both as being part of the same batch.
				if ( ( root_reads_accum[ i ] & root_writes_accum[ j ] ).any() || // writes from j touch reads from i
				     ( root_reads_accum[ j ] & root_writes_accum[ i ] ).any() )  // or writes from i touch reads from j
				{

					// Overlap detectd:
					logger.info( "RenderGraph trees with roots %d and %d are not isolated and must be combined", i, j );

					// Combine queue ids:
					// Get current queue ids for i, j,
					// and combine them in the queue pointed to with the lowest offset

					le::RootPassesField combined_queue_id = queue_id[ queue_id_idx[ j ] ] | queue_id[ queue_id_idx[ i ] ];

					if ( queue_id_idx[ i ] <= queue_id_idx[ j ] ) {
						queue_id_idx[ j ] = queue_id_idx[ i ];
					} else {
						queue_id_idx[ i ] = queue_id_idx[ j ];
					}

					// update queue_id at newly shared queue position
					queue_id[ queue_id_idx[ i ] ] = combined_queue_id;
				}
			}
		}

		// Remove any duplicate entries in our indirection table
		// (if trees get combined they will share the same id)
		auto it = std::unique( queue_id_idx.begin(), queue_id_idx.end() );
		queue_id_idx.erase( it, queue_id_idx.end() );

		// ---------| invariant: queue_id_idx has unique entries,

		// consolidate queue invocation keys, and at the same time test the assertion that no key overlaps
		//
		le::RootPassesField check_queue_accum = 0;
		for ( size_t i = 0; i != queue_id_idx.size(); i++ ) {

#if ( LE_PRINT_DEBUG_MESSAGES )
			logger.info( "queue key [ %-12d], affinity: %x", i, queue_id[ queue_id_idx[ i ] ] );
#endif

			self->root_passes_affinity_masks.push_back( queue_id[ queue_id_idx[ i ] ] );

			{
				// Do some error checking: each bit in the RootPassesField bitfield is only allowed
				// to be used exactly once.
				le::RootPassesField q = queue_id[ queue_id_idx[ i ] ];
				assert( !( q & check_queue_accum ) && "queue lanes must be independent." );
				check_queue_accum |= q;
			}
		}
	}

#if ( LE_GENERATE_DOT_GRAPH )
	{
		// We must check if the renderpass has somehow changed - if we detect change, save out a new .dot file.

		// For the hash, we don't need it to be perfect, we just want to make sure that
		// whenever something might have changed within the rendergraph, we generate a new .dot file.

		// calculate hash over all unique resources
		// calculate hash over all nodes, their signatures

		std::hash<ResourceField> bit_hash;

		std::vector<size_t> node_hashes;
		node_hashes.reserve( nodes.size() * 2 );

		for ( auto& t : nodes ) {
			node_hashes.emplace_back( bit_hash( t.reads ) );
			node_hashes.emplace_back( bit_hash( t.writes ) );
		}

		uint64_t nodes_hash = SpookyHash::Hash64( node_hashes.data(), sizeof( size_t ) * node_hashes.size(), 0 );
		SpookyHash::Hash64( uniqueHandles.data(), sizeof( le_resource_handle_t ) * numUniqueResources, nodes_hash );

		static uint64_t previous_hash = 0;

		if ( previous_hash != nodes_hash ) {
			generate_dot_file_for_rendergraph( self, uniqueHandles.data(), numUniqueResources, nodes.data(), frame_number );
			previous_hash = nodes_hash;
		}
	}
#endif

	{
		// Remove any passes from rendergraph which do not contribute.
		//
		//
		size_t num_passes = self->passes.size();

		std::vector<le_renderpass_o*> consolidated_passes;
		consolidated_passes.reserve( num_passes );

		for ( size_t i = 0; i != num_passes; i++ ) {
			if ( nodes[ i ].is_contributing ) {
				// Pass contributes, add it to consolidated passes
				self->passes[ i ]->is_root              = nodes[ i ].is_root;
				self->passes[ i ]->root_passes_affinity = nodes[ i ].root_index_affinity;
				consolidated_passes.push_back( self->passes[ i ] );
			} else {
				// Pass is not contributing, we will not keep it.
				// Since the rendergraph owns this pass at this point,
				// we must explicitly delete it.
				delete self->passes[ i ];
				self->passes[ i ] = nullptr;
			}
		}

		// Update self->passes
		std::swap( self->passes, consolidated_passes );

#if ( LE_PRINT_DEBUG_MESSAGES )
		logger.info( "* Consolidated Pass List *" );
		int i = 0;
		for ( auto const& p : self->passes ) {
			logger.info( "Pass : %3d : %s ", i, p->debugName );
			i++;
		}
		logger.info( "" );
#endif
	}
}

// ----------------------------------------------------------------------
/// Record commands by calling execution callbacks for each renderpass.
///
/// Commands are stored as a command stream. This command stream uses a binary,
/// API-agnostic representation, and contains an ordered list of commands, and optionally,
/// inlined parameters for each command.
///
/// The command stream is stored inside of the Encoder that is used to record it (that's not elegant).
///
/// We could possibly go wide when recording renderpasses, with one context per renderpass.
static void rendergraph_execute( le_rendergraph_o* self, size_t frameIndex, le_backend_o* backend ) {

	static auto logger = LeLog( LOGGER_LABEL );

	if ( LE_PRINT_DEBUG_MESSAGES ) {
		std::ostringstream msg;
		logger.info( "Render graph: " );
		for ( const auto& pass : self->passes ) {

			logger.info( "Renderpass: '%s'", pass->debugName );
			le_image_attachment_info_t const* pImageAttachments   = nullptr;
			le_img_resource_handle const*     pResources          = nullptr;
			size_t                            numImageAttachments = 0;
			renderpass_get_image_attachments( pass, &pImageAttachments, &pResources, &numImageAttachments );

			for ( size_t i = 0; i != numImageAttachments; ++i ) {
				logger.info( "\t Attachment: '%s' [%10s | %10s]",
				             pResources[ i ]->data->debug_name, //"', last written to in pass: '" << pass_id_to_handle[ attachment->source_id ] << "'"
				             to_str( pImageAttachments[ i ].loadOp ),
				             to_str( pImageAttachments[ i ].storeOp ) );
			}
		}
		logger.info( "" );
	}

	using namespace le_renderer;
	using namespace le_backend_vk;

	// Receive one allocator per renderer worker thread -
	// allocators come from the frame's own pool
	auto const ppAllocators = vk_backend_i.get_transient_allocators( backend, frameIndex );

	auto stagingAllocator = vk_backend_i.get_staging_allocator( backend, frameIndex );

	le_pipeline_manager_o* pipelineCache = vk_backend_i.get_pipeline_cache( backend ); // TODO: make pipeline cache either pass- or frame- local

	// Grab main swapchain dimensions so that we may use these as defaults for
	// encoder extents if these cannot be initialised via renderpass extents.
	//
	// Note that this does not change the renderpass extents.
	// le::Extent2D swapchain_extent{};

	uint32_t num_swapchain_images = 3; // gets updated as a side-effect of backend_i.get_swapchain_info()

	std::vector<le_img_resource_handle> swapchain_images;
	std::vector<uint32_t>               swapchain_image_width;
	std::vector<uint32_t>               swapchain_image_height;

	do {
		swapchain_images.resize( num_swapchain_images );
		swapchain_image_height.resize( num_swapchain_images );
		swapchain_image_width.resize( num_swapchain_images );
	} while ( false ==
	          vk_backend_i.get_swapchain_info(
	              backend,
	              &num_swapchain_images,
	              swapchain_image_width.data(),
	              swapchain_image_height.data(),
	              swapchain_images.data() ) );

	// --------| invariant: - num_swapchain_images holds correct number of swapchain images,
	//                      - swapchain image info is available in swapchain_image[s|_width|_height]

	auto find_matching_resource =
	    []( std::vector<le_img_resource_handle> const& attachments,
	        std::vector<le_img_resource_handle> const& resources,
	        const uint32_t&                            num_resources ) -> uint32_t {
		for ( auto const& attachment : attachments ) {
			for ( uint32_t j = 0; j != num_resources; j++ ) {
				if ( resources[ j ] == attachment ) {
					return j;
					break;
				}
			}
		}
		return 0;
	};

	// Create one encoder per pass, and then record commands by calling the execute callback.

	const size_t numPasses = self->passes.size();

	for ( size_t i = 0; i != numPasses; ++i ) {
		auto& pass = self->passes[ i ];

		if ( !pass->executeCallbacks.empty() ) {

			le::Extent2D pass_extents{
			    pass->width,
			    pass->height,
			};

			if ( pass_extents.width == 0 || pass_extents.height == 0 ) {
				// we must infer pass width and pass height

				// check if any of our pass image attachments matches a swapchain resource
				uint32_t matching_swapchain_idx = find_matching_resource( pass->attachmentResources, swapchain_images, num_swapchain_images ); // default to zero

				pass->width = pass_extents.width = swapchain_image_width[ matching_swapchain_idx ];
				pass->height = pass_extents.height = swapchain_image_height[ matching_swapchain_idx ];
			}

			pass->encoder = encoder_i.create( ppAllocators, pipelineCache, stagingAllocator, pass_extents ); // NOTE: we must manually track the lifetime of encoder!

			if ( pass->type == le::QueueFlagBits::eGraphics ) {

				// Set default scissor and viewport to full extent.

				le::Rect2D default_scissor[ 1 ] = {
				    { 0, 0, pass_extents.width, pass_extents.height },
				};

				le::Viewport default_viewport[ 1 ] = {
				    { 0.f, 0.f, float( pass_extents.width ), float( pass_extents.height ), 0.f, 1.f },
				};

				// setup encoder default viewport and scissor to extent
				encoder_i.set_scissor( pass->encoder, 0, 1, default_scissor );
				encoder_i.set_viewport( pass->encoder, 0, 1, default_viewport );
			}

			renderpass_run_execute_callbacks( pass ); // record draw commands into encoder
		}
	}

	// TODO: consolidate pipeline caches
}

// ----------------------------------------------------------------------

static void rendergraph_get_passes( le_rendergraph_o* self, le_renderpass_o*** pPasses, size_t* pNumPasses ) {
	*pPasses    = self->passes.data();
	*pNumPasses = self->passes.size();
}

// ----------------------------------------------------------------------

static void rendergraph_get_declared_resources( le_rendergraph_o* self, le_resource_handle const** p_resource_handles, le_resource_info_t const** p_resource_infos, size_t* p_resource_count ) {
	*p_resource_count   = self->declared_resources_id.size();
	*p_resource_handles = self->declared_resources_id.data();
	*p_resource_infos   = self->declared_resources_info.data();
}

// ----------------------------------------------------------------------

static void rendergraph_get_p_affinity_masks( le_rendergraph_o* self, le::RootPassesField const** p_affinity_masks, uint32_t* num_affinity_masks ) {
	*p_affinity_masks   = self->root_passes_affinity_masks.data();
	*num_affinity_masks = self->root_passes_affinity_masks.size();
}
// ----------------------------------------------------------------------
// Builds rendergraph from render_module, calls `setup` callbacks on each renderpass which provides a
// `setup` callback.
// If renderpass provides a setup method, pass is only added to rendergraph if its setup
// method returns true. Discards contents of render_module at end.
static void rendergraph_setup_passes( le_rendergraph_o* src_rendergraph, le_rendergraph_o* dst_rendergraph ) {

	for ( auto& pass : src_rendergraph->passes ) {
		// Call setup function on all passes, in order of addition to module
		//
		// Setup Function must:
		// + populate input attachments
		// + populate output attachments
		// + (optionally) add renderpass to graph builder.

		if ( renderpass_has_setup_callback( pass ) ) {
			if ( renderpass_run_setup_callback( pass ) ) {
				// if pass.setup() returns true, this means we shall add this pass to the graph
				// This means a transfer of ownership for pass: pass moves from into graph_builder
				dst_rendergraph->passes.push_back( pass );
				pass = nullptr;
			} else {
				renderpass_destroy( pass );
				pass = nullptr;
			}
		} else {
			dst_rendergraph->passes.push_back( pass );
			pass = nullptr;
		}
	}

	// Move any resource ids and resource infos from module into rendergraph
	dst_rendergraph->declared_resources_id   = std::move( src_rendergraph->declared_resources_id );
	dst_rendergraph->declared_resources_info = std::move( src_rendergraph->declared_resources_info );

	src_rendergraph->passes.clear();
};

// ----------------------------------------------------------------------

static void rendergraph_declare_resource( le_rendergraph_o* self, le_resource_handle const& resource_id, le_resource_info_t const& info ) {
	self->declared_resources_id.emplace_back( resource_id );
	self->declared_resources_info.emplace_back( info );
}

// ----------------------------------------------------------------------

void register_le_rendergraph_api( void* api_ ) {

	auto le_renderer_api_i = static_cast<le_renderer_api*>( api_ );

	auto& le_rendergraph_i            = le_renderer_api_i->le_rendergraph_i;
	le_rendergraph_i.create           = rendergraph_create;
	le_rendergraph_i.destroy          = rendergraph_destroy;
	le_rendergraph_i.reset            = rendergraph_reset;
	le_rendergraph_i.add_renderpass   = rendergraph_add_renderpass;
	le_rendergraph_i.declare_resource = rendergraph_declare_resource;

	auto& le_rendergraph_private_i                  = le_renderer_api_i->le_rendergraph_private_i;
	le_rendergraph_private_i.setup_passes           = rendergraph_setup_passes;
	le_rendergraph_private_i.build                  = rendergraph_build;
	le_rendergraph_private_i.execute                = rendergraph_execute;
	le_rendergraph_private_i.get_passes             = rendergraph_get_passes;
	le_rendergraph_private_i.get_declared_resources = rendergraph_get_declared_resources;
	le_rendergraph_private_i.get_p_affinity_masks   = rendergraph_get_p_affinity_masks;

	auto& le_renderpass_i                        = le_renderer_api_i->le_renderpass_i;
	le_renderpass_i.create                       = renderpass_create;
	le_renderpass_i.clone                        = renderpass_clone;
	le_renderpass_i.destroy                      = renderpass_destroy;
	le_renderpass_i.get_id                       = renderpass_get_id;
	le_renderpass_i.get_debug_name               = renderpass_get_debug_name;
	le_renderpass_i.get_queue_sumbission_info    = renderpass_get_queue_submission_info;
	le_renderpass_i.get_framebuffer_settings     = renderpass_get_framebuffer_settings;
	le_renderpass_i.set_width                    = renderpass_set_width;
	le_renderpass_i.set_sample_count             = renderpass_set_sample_count;
	le_renderpass_i.set_height                   = renderpass_set_height;
	le_renderpass_i.set_setup_callback           = renderpass_set_setup_callback;
	le_renderpass_i.has_setup_callback           = renderpass_has_setup_callback;
	le_renderpass_i.set_execute_callback         = renderpass_set_execute_callback;
	le_renderpass_i.has_execute_callback         = renderpass_has_execute_callback;
	le_renderpass_i.set_is_root                  = renderpass_set_is_root;
	le_renderpass_i.get_is_root                  = renderpass_get_is_root;
	le_renderpass_i.add_color_attachment         = renderpass_add_color_attachment;
	le_renderpass_i.add_depth_stencil_attachment = renderpass_add_depth_stencil_attachment;
	le_renderpass_i.get_image_attachments        = renderpass_get_image_attachments;
	le_renderpass_i.use_resource                 = renderpass_use_resource;
	le_renderpass_i.get_used_resources           = renderpass_get_used_resources;
	le_renderpass_i.steal_encoder                = renderpass_steal_encoder;
	le_renderpass_i.sample_texture               = renderpass_sample_texture;
	le_renderpass_i.get_texture_ids              = renderpass_get_texture_ids;
	le_renderpass_i.get_texture_infos            = renderpass_get_texture_infos;
	le_renderpass_i.ref_inc                      = renderpass_ref_inc;
	le_renderpass_i.ref_dec                      = renderpass_ref_dec;
}
