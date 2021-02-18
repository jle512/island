#include "le_log.h"
#include "le_core/le_core.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdarg>

struct le_log_channel_o {
	std::string     name      = "DEFAULT";
	std::atomic_int log_level = 1;
};

struct le_log_context_o {
	le_log_channel_o                                    channel_default;
	std::unordered_map<std::string, le_log_channel_o *> channels;
	std::mutex                                          mtx;
};

static le_log_context_o *ctx;

static le_log_channel_o *le_log_channel_default() {
	return &ctx->channel_default;
}

static le_log_channel_o *le_log_get_module( const char *name ) {
	if ( !name || !name[ 0 ] ) {
		return le_log_channel_default();
	}

	std::scoped_lock g( ctx->mtx );
	if ( ctx->channels.find( name ) == ctx->channels.end() ) {
		auto module           = new le_log_channel_o();
		module->name          = name;
		ctx->channels[ name ] = module;
		return module;
	}
	return ctx->channels[ name ];
}

static void le_log_set_level( le_log_channel_o *channel, LeLog::Level level ) {
	if ( !channel ) {
		channel = le_log_channel_default();
	}
	channel->log_level = static_cast<std::underlying_type<LeLog::Level>::type>( level );
}

static const char *le_log_level_name( LeLog::Level level ) {
	switch ( level ) {
	case LeLog::Level::DEBUG:
		return "DEBUG";
	case LeLog::Level::INFO:
		return "INFO";
	case LeLog::Level::WARN:
		return "WARN";
	case LeLog::Level::ERROR:
		return "ERROR";
	}
	return "";
}

static void le_log_printf( const le_log_channel_o *channel, LeLog::Level level, const char *msg, va_list args ) {

	if ( !channel ) {
		channel = le_log_channel_default();
	}

	if ( int( level ) < channel->log_level ) {
		return;
	}

	auto file = stdout;

	if ( level == LeLog::Level::ERROR ) {
		file = stderr;
	}

	fprintf( file, "[ %-10s | %-7s ] ", channel->name.c_str(), le_log_level_name( level ) );

	vfprintf( file, msg, args );
	fprintf( file, "\n" );

	fflush( file );
}

template <LeLog::Level level>
static void le_log_implementation( const le_log_channel_o *channel, const char *msg, ... ) {
	va_list arglist;
	va_start( arglist, msg );
	le_log_printf( channel, level, msg, arglist );
	va_end( arglist );
}

// ----------------------------------------------------------------------

LE_MODULE_REGISTER_IMPL( le_log, api ) {
	auto le_api         = static_cast<le_log_api *>( api );
	le_api->get_channel = le_log_get_module;

	auto &le_api_channel_i     = le_api->le_log_channel_i;
	le_api_channel_i.debug     = le_log_implementation<LeLog::Level::DEBUG>;
	le_api_channel_i.info      = le_log_implementation<LeLog::Level::INFO>;
	le_api_channel_i.warn      = le_log_implementation<LeLog::Level::WARN>;
	le_api_channel_i.error     = le_log_implementation<LeLog::Level::ERROR>;
	le_api_channel_i.set_level = le_log_set_level;

	if ( !le_api->context ) {
		le_api->context = new le_log_context_o();
	}

	ctx = le_api->context;
}