#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/unit_test.hpp>

#include "serial/port_types/network_serial_port_impl.h"
#include "serial/rfc2217/rfc2217_constants.h"
#include "serial/serial_port_enums.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Serial::PortTypes;

namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

//=============================================================================
// NetworkSerialPortImpl (serial-over-TCP) against an in-process loopback
// acceptor: construction (raw vs RFC2217), open/close/cancel state handling,
// the non-blocking read/write paths (would_block, data, peer disconnect) and
// the RFC2217 inbound IAC filter.  No real device, no network beyond 127.0.0.1.
//=============================================================================

namespace
{
	// A listening loopback socket the port under test connects to.  Accept()
	// hands back the server side of the connection.
	struct LoopbackListener
	{
		asio::io_context io;
		tcp::acceptor acceptor{ io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0) };

		std::string Endpoint() const
		{
			return std::format("127.0.0.1:{}", acceptor.local_endpoint().port());
		}

		tcp::socket Accept()
		{
			tcp::socket server(io);
			acceptor.accept(server);
			return server;
		}
	};

	// Reserve then release a loopback port so nothing listens on it.
	std::uint16_t DeadPort(asio::io_context& io)
	{
		tcp::acceptor probe(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
		return probe.local_endpoint().port();
	}

	// Poll the (non-blocking) port until a read yields data, the port reports
	// closed, or a bounded number of attempts passes.  Returns the bytes read.
	std::vector<uint8_t> ReadWithRetry(NetworkSerialPortImpl& port, boost::system::error_code& ec, std::size_t capacity = 64)
	{
		std::vector<uint8_t> buffer(capacity);
		for (int attempt = 0; attempt < 2000; ++attempt)
		{
			const auto n = port.read_some(asio::buffer(buffer), ec);
			if ((n > 0) || ec || !port.is_open())
			{
				buffer.resize(n);
				return buffer;
			}
			std::this_thread::yield();
		}
		buffer.clear();
		return buffer;
	}

	// Blocking read on the server side of an already-connected loopback socket.
	std::vector<uint8_t> ServerRead(tcp::socket& server, std::size_t capacity = 64)
	{
		std::vector<uint8_t> buffer(capacity);
		boost::system::error_code ec;
		const auto n = server.read_some(asio::buffer(buffer), ec);
		buffer.resize(ec ? 0 : n);
		return buffer;
	}
}
// anonymous namespace

BOOST_AUTO_TEST_SUITE(NetworkSerialPortImpl_TestSuite)

// -----------------------------------------------------------------------------
// Closed-port behaviour (no connection needed)
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(RawTransport_ClosedPort_AllOperationsReportBadDescriptor)
{
	asio::io_context io;
	NetworkSerialPortImpl port(io.get_executor(), /*use_rfc2217=*/false);

	BOOST_CHECK(!port.is_open());

	boost::system::error_code ec;
	std::array<uint8_t, 8> buffer{};

	BOOST_CHECK_EQUAL(port.read_some(asio::buffer(buffer), ec), 0u);
	BOOST_CHECK(ec == asio::error::bad_descriptor);

	ec = {};
	BOOST_CHECK_EQUAL(port.write_some(asio::buffer(buffer), ec), 0u);
	BOOST_CHECK(ec == asio::error::bad_descriptor);

	ec = {};
	port.cancel(ec);
	BOOST_CHECK(ec == asio::error::bad_descriptor);

	// Throwing overloads surface the same condition as system_error.
	BOOST_CHECK_THROW(port.read_some(asio::buffer(buffer)), boost::system::system_error);
	BOOST_CHECK_THROW(port.write_some(asio::buffer(buffer)), boost::system::system_error);
	BOOST_CHECK_THROW(port.cancel(), boost::system::system_error);

	// Closing a closed port is harmless in both overloads.
	ec = asio::error::fault;
	port.close(ec);
	BOOST_CHECK(!ec);
	BOOST_CHECK_NO_THROW(port.close());
}

BOOST_AUTO_TEST_CASE(RawTransport_PortSettings_WithoutHandler_ClearErrorCode)
{
	// Raw TCP installs no RFC2217 handler: every setter warns and succeeds.
	asio::io_context io;
	NetworkSerialPortImpl port(io.get_executor(), /*use_rfc2217=*/false);

	boost::system::error_code ec = asio::error::fault;
	port.set_baud_rate(9600, ec);
	BOOST_CHECK(!ec);

	ec = asio::error::fault;
	port.set_character_size(8, ec);
	BOOST_CHECK(!ec);

	ec = asio::error::fault;
	port.set_flow_control(Serial::FlowControl::None, ec);
	BOOST_CHECK(!ec);

	ec = asio::error::fault;
	port.set_parity(Serial::Parity::Odd, ec);
	BOOST_CHECK(!ec);

	ec = asio::error::fault;
	port.set_stop_bits(Serial::StopBits::Two, ec);
	BOOST_CHECK(!ec);

	// set_read_timeout is the one option that touches the real socket (setsockopt), so
	// unlike the pure book-keeping setters above it refuses on a port that was never
	// opened rather than silently succeeding.
	ec = asio::error::fault;
	port.set_read_timeout(std::chrono::milliseconds(100), ec);
	BOOST_CHECK(ec == boost::asio::error::bad_descriptor);
}

// -----------------------------------------------------------------------------
// open() failure modes
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Open_InvalidEndpointFormat_ReportsInvalidArgument)
{
	asio::io_context io;
	NetworkSerialPortImpl port(io.get_executor());

	for (const auto* endpoint : { "", "hostonly", ":1234" })
	{
		boost::system::error_code ec;
		port.open(endpoint, ec);
		BOOST_CHECK_MESSAGE(ec == asio::error::invalid_argument, "endpoint '" << endpoint << "' should be rejected");
		BOOST_CHECK(!port.is_open());
	}

	// The throwing overload converts the error into a system_error.
	BOOST_CHECK_THROW(port.open("hostonly"), boost::system::system_error);
}

BOOST_AUTO_TEST_CASE(Open_ConnectionRefused_ReportsErrorAndStaysClosed)
{
	asio::io_context io;
	const auto dead = DeadPort(io);

	NetworkSerialPortImpl port(io.get_executor(), /*use_rfc2217=*/false);

	boost::system::error_code ec;
	port.open(std::format("127.0.0.1:{}", dead), ec);
	BOOST_CHECK(ec);
	BOOST_CHECK(!port.is_open());
}

BOOST_AUTO_TEST_CASE(EndpointConstructors_PropagateOpenOutcome)
{
	asio::io_context io;
	const auto dead = DeadPort(io);
	const auto endpoint = std::format("127.0.0.1:{}", dead);

	// error_code constructor: the failure is reported, not thrown.
	{
		boost::system::error_code ec;
		NetworkSerialPortImpl port(io.get_executor(), endpoint, ec, /*use_rfc2217=*/false);
		BOOST_CHECK(ec);
		BOOST_CHECK(!port.is_open());
	}

	// Throwing constructor: the same failure escapes as system_error.
	BOOST_CHECK_THROW((NetworkSerialPortImpl{ io.get_executor(), endpoint, false }), boost::system::system_error);

	// ... and against a live listener the throwing constructor opens the port.
	LoopbackListener listener;
	NetworkSerialPortImpl connected(listener.io.get_executor(), listener.Endpoint(), /*use_rfc2217=*/false);
	BOOST_CHECK(connected.is_open());
	auto server = listener.Accept();
	BOOST_CHECK(server.is_open());
}

// -----------------------------------------------------------------------------
// Raw TCP data path
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(RawTransport_ReadWriteRoundTrip_AndWouldBlockIsBenign)
{
	LoopbackListener listener;
	NetworkSerialPortImpl port(listener.io.get_executor(), /*use_rfc2217=*/false);

	boost::system::error_code ec;
	port.open(listener.Endpoint(), ec);
	BOOST_REQUIRE_MESSAGE(!ec, ec.message());
	BOOST_REQUIRE(port.is_open());

	auto server = listener.Accept();

	// Nothing has been sent yet: a non-blocking read is a benign zero-byte
	// result with a CLEARED error code in both overloads.
	std::array<uint8_t, 16> buffer{};
	ec = asio::error::fault;
	BOOST_CHECK_EQUAL(port.read_some(asio::buffer(buffer), ec), 0u);
	BOOST_CHECK(!ec);
	std::size_t throwing_read = 1;
	BOOST_CHECK_NO_THROW(throwing_read = port.read_some(asio::buffer(buffer)));
	BOOST_CHECK_EQUAL(throwing_read, 0u);
	BOOST_CHECK(port.is_open());

	// Raw transport: 0xFF (telnet IAC) passes straight through unfiltered.
	const std::array<uint8_t, 4> from_device{ 0x10, 0x02, 0xFF, 0x03 };
	asio::write(server, asio::buffer(from_device));

	const auto received = ReadWithRetry(port, ec);
	BOOST_CHECK(!ec);
	BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), from_device.begin(), from_device.end());

	// Write path (throwing overload): bytes land on the server side verbatim.
	const std::array<uint8_t, 3> to_device{ 0xAA, 0xBB, 0xCC };
	BOOST_CHECK_EQUAL(port.write_some(asio::buffer(to_device)), 3u);
	const auto server_saw = ServerRead(server);
	BOOST_CHECK_EQUAL_COLLECTIONS(server_saw.begin(), server_saw.end(), to_device.begin(), to_device.end());

	// Already open: a second open is refused without disturbing the connection.
	port.open(listener.Endpoint(), ec);
	BOOST_CHECK(ec == asio::error::already_open);
	BOOST_CHECK(port.is_open());

	// cancel() on an open port succeeds.
	ec = asio::error::fault;
	port.cancel(ec);
	BOOST_CHECK(!ec);
	BOOST_CHECK_NO_THROW(port.cancel());

	// Explicit close tears the socket down; the peer observes EOF.
	BOOST_CHECK_NO_THROW(port.close());
	BOOST_CHECK(!port.is_open());
	BOOST_CHECK(ServerRead(server).empty());
}

BOOST_AUTO_TEST_CASE(RawTransport_PeerDisconnect_DuringRead_MarksPortClosedOnce)
{
	LoopbackListener listener;
	NetworkSerialPortImpl port(listener.io.get_executor(), /*use_rfc2217=*/false);

	boost::system::error_code ec;
	port.open(listener.Endpoint(), ec);
	BOOST_REQUIRE(!ec);
	auto server = listener.Accept();

	// Peer goes away (graceful close): the next read observes EOF, the port
	// marks itself closed so the engine can reconnect, and reports 0 bytes.
	boost::system::error_code ignored;
	server.shutdown(tcp::socket::shutdown_both, ignored);
	server.close(ignored);

	const auto bytes = ReadWithRetry(port, ec);
	BOOST_CHECK(bytes.empty());
	BOOST_CHECK(ec);
	BOOST_CHECK(!port.is_open());

	// Subsequent reads report the closed descriptor instead of re-logging.
	std::array<uint8_t, 8> buffer{};
	ec = {};
	BOOST_CHECK_EQUAL(port.read_some(asio::buffer(buffer), ec), 0u);
	BOOST_CHECK(ec == asio::error::bad_descriptor);

	// The port can be re-opened afterwards (reconnect path).
	port.open(listener.Endpoint(), ec);
	BOOST_CHECK(!ec);
	BOOST_CHECK(port.is_open());
}

BOOST_AUTO_TEST_CASE(RawTransport_PeerReset_DuringWrite_MarksPortClosed)
{
	LoopbackListener listener;
	NetworkSerialPortImpl port(listener.io.get_executor(), /*use_rfc2217=*/false);

	boost::system::error_code ec;
	port.open(listener.Endpoint(), ec);
	BOOST_REQUIRE(!ec);
	auto server = listener.Accept();

	// Leave unread data in the peer's receive queue so its close sends a RST,
	// then keep writing until the hard error is observed.
	const std::array<uint8_t, 2> first{ 0x01, 0x02 };
	BOOST_CHECK_EQUAL(port.write_some(asio::buffer(first), ec), 2u);
	BOOST_CHECK(!ec);

	boost::system::error_code ignored;
	server.close(ignored);

	const std::vector<uint8_t> payload(1024, 0x5A);
	bool saw_error = false;
	for (int attempt = 0; (attempt < 2000) && !saw_error; ++attempt)
	{
		ec = {};
		const auto n = port.write_some(asio::buffer(payload), ec);
		if (ec || !port.is_open())
		{
			saw_error = true;
			BOOST_CHECK_EQUAL(n, 0u);
		}
		else
		{
			std::this_thread::yield();
		}
	}

	BOOST_CHECK(saw_error);
	BOOST_CHECK(!port.is_open());

	// Once marked closed the write path reports the closed descriptor.
	ec = {};
	BOOST_CHECK_EQUAL(port.write_some(asio::buffer(payload), ec), 0u);
	BOOST_CHECK(ec == asio::error::bad_descriptor);
}

// -----------------------------------------------------------------------------
// RFC2217 transport
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Rfc2217Transport_Open_SendsNegotiation_AndFiltersInboundIAC)
{
	LoopbackListener listener;
	NetworkSerialPortImpl port(listener.io.get_executor());   // RFC2217 by default

	boost::system::error_code ec;
	port.open(listener.Endpoint(), ec);
	BOOST_REQUIRE_MESSAGE(!ec, ec.message());
	BOOST_REQUIRE(port.is_open());

	auto server = listener.Accept();

	// The handler negotiates on open: the first bytes the "device" sees are the
	// telnet WILL COM-PORT-OPTION request.
	const auto negotiation = ServerRead(server, 256);
	BOOST_REQUIRE_GE(negotiation.size(), 3u);
	BOOST_CHECK_EQUAL(static_cast<int>(negotiation[0]), static_cast<int>(Serial::RFC2217::Constants::IAC));
	BOOST_CHECK_EQUAL(static_cast<int>(negotiation[1]), static_cast<int>(Serial::RFC2217::Constants::WILL));
	BOOST_CHECK_EQUAL(static_cast<int>(negotiation[2]), static_cast<int>(Serial::RFC2217::Constants::COM_PORT_OPTION));

	// Port settings are forwarded to the handler as RFC2217 commands (each is a
	// further IAC-framed write), and the error code is cleared.
	ec = asio::error::fault;
	port.set_baud_rate(19200, ec);
	BOOST_CHECK(!ec);
	ec = asio::error::fault;
	port.set_character_size(8, ec);
	BOOST_CHECK(!ec);
	ec = asio::error::fault;
	port.set_parity(Serial::Parity::None, ec);
	BOOST_CHECK(!ec);
	ec = asio::error::fault;
	port.set_stop_bits(Serial::StopBits::One, ec);
	BOOST_CHECK(!ec);
	ec = asio::error::fault;
	port.set_flow_control(Serial::FlowControl::None, ec);
	BOOST_CHECK(!ec);

	// Inbound: an escaped IAC (FF FF) decodes to a single 0xFF data byte and a
	// telnet command sequence (IAC WILL COM-PORT-OPTION) is consumed entirely.
	const std::array<uint8_t, 8> wire{ 0x10, 0x02, 0xFF, 0xFF,
		Serial::RFC2217::Constants::IAC, Serial::RFC2217::Constants::WILL, Serial::RFC2217::Constants::COM_PORT_OPTION, 0x03 };
	asio::write(server, asio::buffer(wire));

	const auto received = ReadWithRetry(port, ec);
	BOOST_CHECK(!ec);
	const std::array<uint8_t, 4> expected{ 0x10, 0x02, 0xFF, 0x03 };
	BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), expected.begin(), expected.end());

	// Close shuts the protocol handler down and releases the socket.
	ec = asio::error::fault;
	port.close(ec);
	BOOST_CHECK(!ec);
	BOOST_CHECK(!port.is_open());
}

BOOST_AUTO_TEST_CASE(Destructor_WithOpenPort_ClosesConnection)
{
	LoopbackListener listener;
	tcp::socket server(listener.io);

	{
		NetworkSerialPortImpl port(listener.io.get_executor(), /*use_rfc2217=*/false);
		boost::system::error_code ec;
		port.open(listener.Endpoint(), ec);
		BOOST_REQUIRE(!ec);
		server = listener.Accept();
		BOOST_CHECK(server.is_open());
		// Destroyed here while open.
	}

	// The peer observes EOF once the port has been torn down.
	BOOST_CHECK(ServerRead(server).empty());
}

BOOST_AUTO_TEST_SUITE_END()
