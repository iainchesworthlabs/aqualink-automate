#pragma once

#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>

#include "logging/logging_channels.h"
#include "logging/logging_severity_levels.h"

namespace AqualinkAutomate::Logging
{
	using Logger = boost::log::sources::severity_channel_logger_mt<Severity, Channel>;
	
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Certificates, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Coroutines, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Developer, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Devices, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Equipment, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Exceptions, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Main, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Messages, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Mqtt, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Navigation, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Options, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Platform, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Profiling, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Protocol, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Scraping, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Serial, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Signals, Logger);
	BOOST_LOG_GLOBAL_LOGGER(GlobalLogger_Web, Logger);

}
// namespace AqualinkAutomate::Logging
