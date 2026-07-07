#pragma once

#include <exception>
#include <source_location>
#include <string>

#include <boost/stacktrace.hpp>

namespace AqualinkAutomate::Exceptions
{

	class GenericAqualinkException : public std::exception
	{
	public:
		explicit GenericAqualinkException(std::string message, std::source_location location = std::source_location::current(), boost::stacktrace::stacktrace trace = boost::stacktrace::stacktrace());

		const char* what() const noexcept override;
		std::string& What();
		const std::string& What() const noexcept;
		const std::source_location& Where() const noexcept;
		const boost::stacktrace::stacktrace& StackTrace() const noexcept;

	private:
		std::string m_ExceptionMessage;
		const std::source_location m_SourceLocation;
		const boost::stacktrace::stacktrace m_StackTrace;
	};

}
// namespace AqualinkAutomate::Exceptions

//
// Exception-class boilerplate generators.
//
// Each derived exception used to be three lines of hand-written, byte-identical
// boilerplate paired with a placeholder static message constant whose source_location
// was captured at the .cpp definition site (so Where() pointed at the exception's own
// file rather than the throw site).  These macros centralise that boilerplate and fix
// the source_location capture point:
//
//   * AQ_DECLARE_EXCEPTION declares the derived type in a header.  The single defaulted
//     `std::source_location` parameter is evaluated at the CALL SITE (i.e. the throw
//     site) because default arguments are substituted at the point of call, so Where()
//     now reports where the exception was actually thrown.  Existing zero-argument
//     throw expressions (e.g. `throw Exceptions::Hub_NotFound();`) compile unchanged.
//
//   * AQ_DEFINE_EXCEPTION provides the matching out-of-line definition in a .cpp, binding
//     the type to a concise, human-readable message that is surfaced verbatim to operators
//     via what()/What() and the top-level Fatal log path.
//
// Types needing an additional caller-supplied message constructor (e.g. WebServerException)
// declare/define that overload explicitly alongside these macros.
//

#define AQ_DECLARE_EXCEPTION(TYPE)                                                                 \
	class TYPE : public ::AqualinkAutomate::Exceptions::GenericAqualinkException                   \
	{                                                                                              \
	public:                                                                                        \
		explicit TYPE(std::source_location location = std::source_location::current());            \
	}

#define AQ_DEFINE_EXCEPTION(TYPE, MESSAGE)                                                         \
	TYPE::TYPE(std::source_location location) :                                                    \
		::AqualinkAutomate::Exceptions::GenericAqualinkException((MESSAGE), location)              \
	{                                                                                              \
		::LogTrace(                                                                                \
			::AqualinkAutomate::Logging::Channel::Exceptions,                                      \
			#TYPE " exception was constructed");                                                   \
	}

//
// Message-carrying variants.  A subsystem exception that must surface a runtime-composed
// message (a file path, an underlying library's error text) declares the caller-supplied
// message overload with AQ_DECLARE_EXCEPTION_WITH_MESSAGE and defines it with
// AQ_DEFINE_EXCEPTION_WITH_MESSAGE.  The source_location still defaults to the throw site.
//

#define AQ_DECLARE_EXCEPTION_WITH_MESSAGE(TYPE)                                                    \
	class TYPE : public ::AqualinkAutomate::Exceptions::GenericAqualinkException                   \
	{                                                                                              \
	public:                                                                                        \
		explicit TYPE(std::string message, std::source_location location = std::source_location::current()); \
	}

#define AQ_DEFINE_EXCEPTION_WITH_MESSAGE(TYPE)                                                     \
	TYPE::TYPE(std::string message, std::source_location location) :                              \
		::AqualinkAutomate::Exceptions::GenericAqualinkException(std::move(message), location)     \
	{                                                                                              \
		::LogTrace(                                                                                \
			::AqualinkAutomate::Logging::Channel::Exceptions,                                      \
			#TYPE " exception was constructed");                                                   \
	}
