#include "le_renderer.h"
#include "le_renderer/private/hash_util.h"

#include "le_backend_vk/le_backend_vk.h"

#include <vector>
#include <string>
#include <list>
#include <assert.h>
#include <algorithm>
#include <unordered_map>
#include <iostream>
#include <iomanip>

#include "le_renderer/private/le_renderer_types.h"

#ifndef PRINT_DEBUG_MESSAGES
#	define PRINT_DEBUG_MESSAGES false
#endif

#define LE_GRAPH_BUILDER_RECURSION_DEPTH 20

// these are some sanity checks for le_renderer_types
static_assert( sizeof( le::CommandHeader ) == sizeof( uint64_t ), "Size of le::CommandHeader must be 64bit" );

using image_attachment_t = le_image_attachment_info_o;

struct le_renderpass_o {

	LeRenderPassType type     = LE_RENDER_PASS_TYPE_UNDEFINED;
	uint32_t         isRoot   = false; // whether pass *must* be processed
	uint64_t         id       = 0;
	uint64_t         sort_key = 0;

	std::vector<uint64_t>                   readResources;
	std::vector<uint64_t>                   writeResources;
	std::vector<uint64_t>                   createResources;
	std::vector<le_resource_info_t>         createResourceInfos; // createResources holds ids at matching index
	std::vector<le_image_attachment_info_o> imageAttachments;

	le_renderer_api::pfn_renderpass_setup_t   callbackSetup              = nullptr;
	le_renderer_api::pfn_renderpass_execute_t callbackExecute            = nullptr;
	void *                                    execute_callback_user_data = nullptr;

	struct le_command_buffer_encoder_o *encoder   = nullptr;
	std::string                         debugName = "";
};

// ----------------------------------------------------------------------

struct le_render_module_o : NoCopy, NoMove {
	std::vector<le_renderpass_o *> passes;
};

// ----------------------------------------------------------------------

struct le_graph_builder_o : NoCopy, NoMove {
	std::vector<le_renderpass_o *>             passes;
	std::vector<le_command_buffer_encoder_o *> encoders;
};

// ----------------------------------------------------------------------

static le_renderpass_o *renderpass_create( const char *renderpass_name, const LeRenderPassType &type_ ) {
	auto self       = new le_renderpass_o();
	self->id        = const_char_hash64( renderpass_name );
	self->type      = type_;
	self->debugName = renderpass_name;
	return self;
}

// ----------------------------------------------------------------------

static le_renderpass_o *renderpass_clone( le_renderpass_o const *rhs ) {
	auto self = new le_renderpass_o();
	*self     = *rhs;
	return self;
}

// ----------------------------------------------------------------------

static void renderpass_destroy( le_renderpass_o *self ) {

	if ( self->encoder ) {
		static auto const &encoder_i = Registry::getApi<le_renderer_api>()->le_command_buffer_encoder_i;
		encoder_i.destroy( self->encoder );
	}

	delete self;
}

// ----------------------------------------------------------------------

static void renderpass_set_setup_fun( le_renderpass_o *self, le_renderer_api::pfn_renderpass_setup_t fun ) {
	self->callbackSetup = fun;
}

// ----------------------------------------------------------------------

static void renderpass_set_execute_callback( le_renderpass_o *self, le_renderer_api::pfn_renderpass_execute_t callback_, void *user_data_ ) {
	self->execute_callback_user_data = user_data_;
	self->callbackExecute            = callback_;
}

// ----------------------------------------------------------------------
static void renderpass_run_execute_callback( le_renderpass_o *self, le_command_buffer_encoder_o *encoder ) {
	self->encoder = encoder; // store encoder
	self->callbackExecute( self->encoder, self->execute_callback_user_data );
}

static bool renderpass_run_setup_callback( le_renderpass_o *self ) {
	return self->callbackSetup( self );
}

// ----------------------------------------------------------------------

static void renderpass_use_resource( le_renderpass_o *self, uint64_t resource_id, uint32_t accessFlags ) {

	if ( accessFlags & le::AccessFlagBits::eRead ) {
		self->readResources.push_back( resource_id );
	}

	if ( accessFlags & le::AccessFlagBits::eWrite ) {
		self->writeResources.push_back( resource_id );
	}
}

// ----------------------------------------------------------------------

static void renderpass_add_image_attachment( le_renderpass_o *self, uint64_t resource_id, le_image_attachment_info_o *info_ ) {

	self->imageAttachments.push_back( *info_ );
	auto &info = self->imageAttachments.back();

	// By default, flag attachment source as being external, if attachment was previously written in this pass,
	// source will be substituted by id of pass which writes to attachment, otherwise the flag will persist and
	// tell us that this attachment must be externally resolved.
	info.source_id   = const_char_hash64( LE_RENDERPASS_MARKER_EXTERNAL );
	info.resource_id = resource_id;

	if ( info.access_flags == le::AccessFlagBits::eReadWrite ) {
		info.loadOp  = LE_ATTACHMENT_LOAD_OP_LOAD;
		info.storeOp = LE_ATTACHMENT_STORE_OP_STORE;
	} else if ( info.access_flags & le::AccessFlagBits::eWrite ) {
		// Write-only means we may be seen as the creator of this resource
		info.source_id = self->id;
	} else if ( info.access_flags & le::AccessFlagBits::eRead ) {
		// TODO: we need to make sure to distinguish between image attachments and texture attachments
		info.loadOp  = LE_ATTACHMENT_LOAD_OP_LOAD;
		info.storeOp = LE_ATTACHMENT_STORE_OP_DONTCARE;
	} else {
		info.loadOp  = LE_ATTACHMENT_LOAD_OP_DONTCARE;
		info.storeOp = LE_ATTACHMENT_STORE_OP_DONTCARE;
	}

	renderpass_use_resource( self, resource_id, info.access_flags );
	//strncpy( info.debugName, name_, sizeof(info.debugName));
}

// ----------------------------------------------------------------------

static void renderpass_create_resource( le_renderpass_o *self, uint64_t resource_id, const le_resource_info_t &info ) {

	self->createResourceInfos.push_back( info );
	self->createResources.push_back( resource_id );

	// Additionally, we introduce this resource to the write resource table,
	// so that it will be considered when building the graph based on dependencies.
	renderpass_use_resource( self, resource_id, le::AccessFlagBits::eWrite );
}

// ----------------------------------------------------------------------

static void renderpass_set_is_root( le_renderpass_o *self, bool isRoot ) {
	self->isRoot = isRoot;
}

static bool renderpass_get_is_root( le_renderpass_o const *self ) {
	return self->isRoot;
}

static void renderpass_set_sort_key( le_renderpass_o *self, uint64_t sort_key ) {
	self->sort_key = sort_key;
}

static uint64_t renderpass_get_sort_key( le_renderpass_o const *self ) {
	return self->sort_key;
}

static LeRenderPassType renderpass_get_type( le_renderpass_o const *self ) {
	return self->type;
}

static void renderpass_get_read_resources( le_renderpass_o const *self, uint64_t const **pReadResources, size_t *count ) {
	*pReadResources = self->readResources.data();
	*count          = self->readResources.size();
}

static void renderpass_get_write_resources( le_renderpass_o const *self, uint64_t const **pWriteResources, size_t *count ) {
	*pWriteResources = self->writeResources.data();
	*count           = self->writeResources.size();
}

static void renderpass_get_create_resources( le_renderpass_o const *self, uint64_t const **pCreateResources, le_resource_info_t const **pResourceInfos, size_t *count ) {
	assert( self->createResourceInfos.size() == self->createResources.size() );

	*pCreateResources = self->createResources.data();
	*pResourceInfos   = self->createResourceInfos.data();
	*count            = self->createResources.size();
}

static const char *renderpass_get_debug_name( le_renderpass_o const *self ) {
	return self->debugName.c_str();
}

static uint64_t renderpass_get_id( le_renderpass_o const *self ) {
	return self->id;
}

static void renderpass_get_image_attachments( const le_renderpass_o *self, const le_image_attachment_info_o **pAttachments, size_t *numAttachments ) {
	*pAttachments   = self->imageAttachments.data();
	*numAttachments = self->imageAttachments.size();
}

static bool renderpass_has_execute_callback( const le_renderpass_o *self ) {
	return self->callbackExecute != nullptr;
}

static bool renderpass_has_setup_callback( const le_renderpass_o *self ) {
	return self->callbackExecute != nullptr;
}

/// @warning Encoder becomes the thief's worry to destroy!
/// @returns null if encoder was already stolen, otherwise a pointer to an encoder object
le_command_buffer_encoder_o *renderpass_steal_encoder( le_renderpass_o *self ) {
	auto result   = self->encoder;
	self->encoder = nullptr;
	return result;
}

// ----------------------------------------------------------------------

static le_graph_builder_o *graph_builder_create() {
	auto obj = new le_graph_builder_o();
	return obj;
}

// ----------------------------------------------------------------------

static void graph_builder_reset( le_graph_builder_o *self ) {

	// we must destroy passes as we have ownership over them.
	for ( auto rp : self->passes ) {
		renderpass_destroy( rp );
	}

	self->passes.clear();
}

// ----------------------------------------------------------------------

static void graph_builder_destroy( le_graph_builder_o *self ) {
	graph_builder_reset( self );
	delete self;
}

// ----------------------------------------------------------------------

static void graph_builder_add_renderpass( le_graph_builder_o *self, le_renderpass_o *renderpass ) {

	self->passes.push_back( renderpass ); // Note: We receive ownership of the pass here. We must destroy it.
}

// ----------------------------------------------------------------------
/// \brief find corresponding output for each input resource
static std::vector<std::vector<uint64_t>> graph_builder_resolve_resource_ids( const std::vector<le_renderpass_o *> &passes ) {

	static auto const &                renderpass_i = Registry::getApi<le_renderer_api>()->le_renderpass_i;
	std::vector<std::vector<uint64_t>> dependenciesPerPass;

	// Rendermodule gives us a pre-sorted list of renderpasses,
	// we use this to resolve attachment aliases. Since Rendermodule is a linear sequence,
	// this means that dependencies for resources are well-defined. It's impossible for
	// two renderpasses using the same resource not to have a clearly defined priority, as
	// the earliest submitted renderpasses of the two will get priority.

	// returns: for each pass, a list of passes which write to resources that this pass uses.

	dependenciesPerPass.reserve( passes.size() );

	// map from resource id -> source pass id
	std::unordered_map<uint64_t, uint64_t, IdentityHash> writeAttachmentTable;

	// We go through passes in module submission order, so that outputs will match later inputs.
	uint64_t passIndex = 0;
	for ( auto const &pass : passes ) {

		size_t          readResourceCount = 0;
		const uint64_t *pReadResources    = nullptr;
		renderpass_get_read_resources( pass, &pReadResources, &readResourceCount );

		std::vector<uint64_t> passesThisPassDependsOn;
		passesThisPassDependsOn.reserve( readResourceCount );

		// We must first look if any of our READ attachments are already present in the attachment table.
		// If so, we update source ids (from table) for each attachment we found.
		for ( auto *resource = pReadResources; resource != pReadResources + readResourceCount; resource++ ) {

			auto foundOutputIt = writeAttachmentTable.find( *resource );
			if ( foundOutputIt != writeAttachmentTable.end() ) {
				passesThisPassDependsOn.emplace_back( foundOutputIt->second );
			}
		}

		dependenciesPerPass.emplace_back( passesThisPassDependsOn );

		// Outputs from current pass overwrite any cached outputs with same name:
		// later inputs with same name will then resolve to the latest version
		// of an output with a particular name.
		{
			size_t          writeResourceCount = 0;
			const uint64_t *pWriteResources    = nullptr;
			renderpass_i.get_write_resources( pass, &pWriteResources, &writeResourceCount );

			for ( const auto *it = pWriteResources; it != pWriteResources + writeResourceCount; it++ ) {
				writeAttachmentTable[ *it ] = passIndex;
			}
		}

		++passIndex;
	}

	return dependenciesPerPass;
}

// ----------------------------------------------------------------------
/// \brief depth-first traversal of graph, following each input back to its corresponding output (source)
static void graph_builder_traverse_passes( const std::vector<std::vector<uint64_t>> &passes,
                                           const uint64_t &                          currentRenderpassId,
                                           const uint32_t                            recursion_depth,
                                           std::vector<uint32_t> &                   sort_order_per_pass ) {

	if ( recursion_depth > LE_GRAPH_BUILDER_RECURSION_DEPTH ) {
		std::cerr << __FUNCTION__ << " : "
		          << "max recursion level reached. check for cycles in render graph" << std::endl;
		return;
	}

	// TODO: how do we deal with external resources?

	{
		// -- Store recursion depth as sort order for this pass if it is
		//    higher than current sort order for this pass.
		//
		// We want the maximum edge distance (one recursion equals one edge) from the root node
		// for each pass, since the max distance makes sure that all resources are available,
		// even resources which have a shorter path.

		uint32_t &currentSortOrder = sort_order_per_pass[ currentRenderpassId ];

		if ( currentSortOrder < recursion_depth ) {
			currentSortOrder = recursion_depth;
		}
	}

	// -- Iterate over all sources
	// As each input tells us its source renderpass, we can look up the provider pass for each source by id
	const std::vector<uint64_t> &sourcePasses = passes.at( currentRenderpassId );
	for ( auto &sourcePass : sourcePasses ) {
		graph_builder_traverse_passes( passes, sourcePass, recursion_depth + 1, sort_order_per_pass );
	}
}

// ----------------------------------------------------------------------

static std::vector<uint64_t> graph_builder_find_root_passes( const std::vector<le_renderpass_o *> &passes ) {

	std::vector<uint64_t> roots;
	roots.reserve( passes.size() );

	uint64_t i = 0;
	for ( auto const &pass : passes ) {
		if ( renderpass_get_is_root( pass ) ) {
			roots.push_back( i );
		}
		++i;
	}

	return roots;
}

// ----------------------------------------------------------------------

static void graph_builder_build_graph( le_graph_builder_o *self ) {

	// Find corresponding output for each input attachment,
	// and tag input with output id, as dependencies are
	// declared using names rather than linked in code.
	auto pass_dependencies = graph_builder_resolve_resource_ids( self->passes );

	{
		// Establish a toplogical sorting order
		// so that passes which produce resources for other
		// passes are executed *before* their dependencies
		//
		auto root_passes = graph_builder_find_root_passes( self->passes );

		std::vector<uint32_t> pass_sort_orders; // sort order for each pass in self->passes
		pass_sort_orders.resize( self->passes.size(), 0 );

		for ( auto root : root_passes ) {
			// note that we begin with sort order 1, so that any passes which have
			// sort order 0 still after this loop is complete can be seen as
			// marked for deletion / or can be ignored.
			graph_builder_traverse_passes( pass_dependencies, root, 1, pass_sort_orders );
		}

		// We use the passes' sort order as a field in the
		// sorting key for any command buffers associated with that
		// renderpass.

		// store sort key with every pass
		for ( size_t i = 0; i != self->passes.size(); ++i ) {
			self->passes[ i ]->sort_key = pass_sort_orders[ i ];
		}
	}

	// -- Eliminate any passes with sort key 0 (they don't contribute)
	auto end_valid_passes_range = std::remove_if( self->passes.begin(), self->passes.end(), []( le_renderpass_o *lhs ) -> bool {
	    bool needs_removal = false;
	    if ( lhs->sort_key == 0 ) {
	        needs_removal = true;
	        renderpass_destroy( lhs );
        }
	    return needs_removal;
    } );

	// remove any passes which were marked invalid
	self->passes.erase( end_valid_passes_range, self->passes.end() );

	// Use sort key to order passes in decending order, based on sort key.
	// pass with lower sort key depends on pass with higher sort key.
	std::stable_sort( self->passes.begin(), self->passes.end(), []( le_renderpass_o const *lhs, le_renderpass_o const *rhs ) {
		return lhs->sort_key > rhs->sort_key;
	} );
}

// ----------------------------------------------------------------------

static void graph_builder_execute_graph( le_graph_builder_o *self, size_t frameIndex, le_backend_o *backend ) {

	/// Record render commands by calling rendercallbacks for each renderpass.
	///
	/// Render Commands are stored as a command stream. This command stream uses a binary,
	/// API-agnostic representation, and contains an ordered list of commands, and optionally,
	/// inlined parameters for each command.
	///
	/// The command stream is stored inside of the Encoder that is used to record it (that's not elegant).
	///
	/// We could possibly go wide when recording renderpasses, with one context per renderpass.

	if ( PRINT_DEBUG_MESSAGES ) {
		std::ostringstream msg;
		msg << "render graph: " << std::endl;
		for ( const auto &pass : self->passes ) {
			msg << "renderpass: " << std::setw( 15 ) << std::hex << pass->id << ", "
			    << "'" << pass->debugName << "' , sort_key: " << pass->sort_key << std::endl;

			le_image_attachment_info_o const *pImageAttachments   = nullptr;
			size_t                            numImageAttachments = 0;
			renderpass_get_image_attachments( pass, &pImageAttachments, &numImageAttachments );

			for ( auto const *attachment = pImageAttachments; attachment != pImageAttachments + numImageAttachments; attachment++ ) {
				if ( attachment->access_flags & le::AccessFlagBits::eRead ) {
					msg << "r";
				}
				if ( attachment->access_flags & le::AccessFlagBits::eWrite ) {
					msg << "w";
				}
				msg << " : " << std::setw( 32 ) << std::hex << attachment->resource_id << ":" << attachment->source_id << ", '" << attachment->debugName << "'" << std::endl;
			}
		}
		std::cout << msg.str();
	}

	self->encoders.reserve( self->passes.size() );

	static const auto &encoderInterface = Registry::getApi<le_renderer_api>()->le_command_buffer_encoder_i;
	static const auto &backendInterface = Registry::getApi<le_backend_vk_api>()->vk_backend_i;

	// Receive one allocator per pass
	auto const       ppAllocators = backendInterface.get_transient_allocators( backend, frameIndex, self->passes.size() );
	le_allocator_o **allocIt      = ppAllocators; // iterator over allocators - note that number of allocators must be identical with number of passes

	for ( auto &pass : self->passes ) {

		//assert( pass.sort_key != 0 ); // passes with sort key 0 must have been removed by now.

		if ( pass->callbackExecute && pass->sort_key != 0 ) {

			auto encoder = encoderInterface.create( *allocIt ); // NOTE: we must manually track the lifetime of encoder!

			renderpass_run_execute_callback( pass, encoder ); // record draw commands into encoder

			allocIt++; // Move to next unused allocator
		}
	}
}

// ----------------------------------------------------------------------

static void graph_builder_get_passes( le_graph_builder_o *self, le_renderpass_o ***pPasses, size_t *pNumPasses ) {
	*pPasses    = self->passes.data();
	*pNumPasses = self->passes.size();
}

// ----------------------------------------------------------------------

static le_render_module_o *render_module_create() {
	auto obj = new le_render_module_o();
	return obj;
}

// ----------------------------------------------------------------------

static void render_module_destroy( le_render_module_o *self ) {
	delete self;
}

// ----------------------------------------------------------------------

// TODO: make sure name for each pass is unique per rendermodule.
static void render_module_add_renderpass( le_render_module_o *self, le_renderpass_o *pass ) {
	// Note: we clone the pass here, as we can't be sure that the original pass will not fall out of scope
	// and be destroyed.
	self->passes.push_back( renderpass_clone( pass ) );
}

// ----------------------------------------------------------------------

static void render_module_setup_passes( le_render_module_o *self, le_graph_builder_o *graph_builder_ ) {

	for ( auto &pass : self->passes ) {
		// Call setup function on all passes, in order of addition to module
		//
		// Setup Function must:
		// + populate input attachments
		// + populate output attachments
		// + (optionally) add renderpass to graph builder.
		assert( renderpass_has_setup_callback( pass ) );

		if ( renderpass_run_setup_callback( pass ) ) {
			// if pass.setup() returns true, this means we shall add this pass to the graph
			// This means a transfer of ownership for pass: pass moves from module into graph_builder
			graph_builder_add_renderpass( graph_builder_, pass );
			pass = nullptr;
		} else {
			renderpass_destroy( pass );
			pass = nullptr;
		}
	}

	self->passes.clear();

	// Now, renderpasses should have their attachments properly set.
	// Further, user will have added all renderpasses they wanted included in the module
	// to the graph builder.

	// The graph builder now has a list of all passes which contribute to the current module.

	// Step 1: Validate
	// - find any name clashes: inputs and outputs for each renderpass must be unique.
	// Step 2: sort passes in dependency order (by adding an execution order index to each pass)
	// Step 3: add  markers to each attachment for each pass, depending on their read/write status
};

// ----------------------------------------------------------------------

void register_le_rendergraph_api( void *api_ ) {

	auto le_renderer_api_i = static_cast<le_renderer_api *>( api_ );

	auto &le_render_module_i          = le_renderer_api_i->le_render_module_i;
	le_render_module_i.create         = render_module_create;
	le_render_module_i.destroy        = render_module_destroy;
	le_render_module_i.add_renderpass = render_module_add_renderpass;
	le_render_module_i.setup_passes   = render_module_setup_passes;

	auto &le_graph_builder_i   = le_renderer_api_i->le_graph_builder_i;
	le_graph_builder_i.create  = graph_builder_create;
	le_graph_builder_i.destroy = graph_builder_destroy;
	le_graph_builder_i.reset   = graph_builder_reset;

	le_graph_builder_i.build_graph   = graph_builder_build_graph;
	le_graph_builder_i.execute_graph = graph_builder_execute_graph;
	le_graph_builder_i.get_passes    = graph_builder_get_passes;

	auto &le_renderpass_i                 = le_renderer_api_i->le_renderpass_i;
	le_renderpass_i.create                = renderpass_create;
	le_renderpass_i.clone                 = renderpass_clone;
	le_renderpass_i.destroy               = renderpass_destroy;
	le_renderpass_i.add_image_attachment  = renderpass_add_image_attachment;
	le_renderpass_i.set_setup_callback    = renderpass_set_setup_fun;
	le_renderpass_i.has_setup_callback    = renderpass_has_setup_callback;
	le_renderpass_i.run_setup_callback    = renderpass_run_setup_callback;
	le_renderpass_i.set_execute_callback  = renderpass_set_execute_callback;
	le_renderpass_i.run_execute_callback  = renderpass_run_execute_callback;
	le_renderpass_i.has_execute_callback  = renderpass_has_execute_callback;
	le_renderpass_i.use_resource          = renderpass_use_resource;
	le_renderpass_i.set_is_root           = renderpass_set_is_root;
	le_renderpass_i.get_is_root           = renderpass_get_is_root;
	le_renderpass_i.get_sort_key          = renderpass_get_sort_key;
	le_renderpass_i.set_sort_key          = renderpass_set_sort_key;
	le_renderpass_i.get_read_resources    = renderpass_get_read_resources;
	le_renderpass_i.get_write_resources   = renderpass_get_write_resources;
	le_renderpass_i.get_create_resources  = renderpass_get_create_resources;
	le_renderpass_i.get_id                = renderpass_get_id;
	le_renderpass_i.get_debug_name        = renderpass_get_debug_name;
	le_renderpass_i.get_image_attachments = renderpass_get_image_attachments;
	le_renderpass_i.get_type              = renderpass_get_type;
	le_renderpass_i.steal_encoder         = renderpass_steal_encoder;
	le_renderpass_i.create_resource       = renderpass_create_resource;
}
