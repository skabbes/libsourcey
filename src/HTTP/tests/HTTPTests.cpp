#include "Sourcey/Application.h"
#include "Sourcey/HTTP/Server.h"
#include "Sourcey/HTTP/Connection.h"
#include "Sourcey/HTTP/Client.h"
#include "Sourcey/HTTP/WebSocket.h"
#include "Sourcey/HTTP/Packetizers.h"
#include "Sourcey/HTTP/URL.h"
#include "Sourcey/Runner.h"
#include "Sourcey/Timer.h"
#include "Sourcey/Idler.h"

#include "Sourcey/Base.h"
#include "Sourcey/Logger.h"
#include "Sourcey/Net/SSLManager.h"
#include "Sourcey/Net/SSLContext.h"
#include "Sourcey/Net/Address.h"

#include "assert.h"
#include <iterator>


using namespace std;
using namespace scy;
using namespace scy::net;


/*
// Detect memory leaks on winders
#if defined(_DEBUG) && defined(_WIN32)
#include "MemLeakDetect/MemLeakDetect.h"
#include "MemLeakDetect/MemLeakDetect.cpp"
CMemLeakDetect memLeakDetect;
#endif
*/


namespace scy {
namespace http {

	
#define TEST_SSL 1 //0
#define TEST_HTTP_PORT 1340
#define TEST_HTTPS_PORT 1338

	
// -------------------------------------------------------------------
//
class BasicResponder: public ServerResponder
{
public:
	BasicResponder(ServerConnection& conn) : 
		ServerResponder(conn)
	{
		debugL("BasicResponder") << "Creating" << endl;
	}

	void onRequest(Request& request, Response& response) 
	{
		debugL("BasicResponder") << "On complete" << endl;

		response.setContentLength(14);

		//connection().sendHeader(); // headers will be flushed
		connection().write("Hello universe", 14);
		connection().close();
	}
};


// -------------------------------------------------------------------
//
struct RandomDataSource: public Idler 
{
	PacketSignal signal;

	virtual void onIdle()
	{
		signal.emit(this, RawPacket("somedata", 8));
	}
};	


class ChunkedResponder: public ServerResponder
{
public:
	RandomDataSource dataSource;
	bool gotHeaders;
	bool gotRequest;
	bool gotClose;

	ChunkedResponder(ServerConnection& conn) : 
		ServerResponder(conn), 
		gotHeaders(false), 
		gotRequest(false), 
		gotClose(false)
	{
		conn.Outgoing.attach(new http::ChunkedAdapter(conn)); //"text/html"
		conn.Outgoing.attachSource(&dataSource.signal, false);
		//dataSource.signal += delegate(&conn.socket(), &Socket::send);
	}

	~ChunkedResponder()
	{
		assert(gotHeaders);
		assert(gotRequest);
		assert(gotClose);
	}

	void onHeaders(Request& request) 
	{
		gotHeaders = true;
	}

	void onRequest(Request& request, Response& response) 
	{
		gotRequest = true;
		
		connection().response().set("Access-Control-Allow-Origin", "*");
		connection().response().set("Content-Type", "text/html");
		connection().response().set("Transfer-Encoding", "chunked");

		// headers pushed through automatically
		//connection().sendHeader();

		// Start shooting data at the client
		dataSource.start();
	}

	void onClose()
	{
		debugL("ChunkedResponder") << "On connection close" << endl;
		gotClose = true;
		dataSource.stop();
	}
};


// -------------------------------------------------------------------
//
class WebSocketResponder: public ServerResponder
{
public:
	bool gotPayload;
	bool gotClose;

	WebSocketResponder(ServerConnection& conn) : 
		ServerResponder(conn), 
		gotPayload(false), 
		gotClose(false)
	{
		debugL("WebSocketResponder") << "Creating" << endl;
	}

	~WebSocketResponder()
	{
		debugL("WebSocketResponder") << "Destroying" << endl;
		assert(gotPayload);
		assert(gotClose);
	}

	void onPayload(const Buffer& body)
	{
		debugL("WebSocketResponder") << "On payload: " << body << endl;

		gotPayload = true;

		// Enco the request back to the client
		connection().write(body.data(), body.available());
	}

	void onClose()
	{
		debugL("WebSocketResponder") << "On connection close" << endl;
		gotClose = true;
	}
};


// -------------------------------------------------------------------
//
class OurServerResponderFactory: public ServerResponderFactory
{
public:
	ServerResponder* createResponder(ServerConnection& conn)
	{		
		ostringstream os;
		conn.request().write(os);
		string headers(os.str().data(), os.str().length());
		debugL("OurServerResponderFactory") << "Incoming Request: " << headers << endl; // remove me

		if (conn.request().getURI() == "/chunked")
			return new ChunkedResponder(conn);
		else if (conn.request().getURI() == "/websocket")
			return new WebSocketResponder(conn);
		else
			return new BasicResponder(conn);
	}
};


// -------------------------------------------------------------------
//
template <typename SocketT>
class SocketClientEchoTest
{
public:
	typename SocketT socket;
	net::Address address;

	SocketClientEchoTest(const net::Address& addr, bool ghost = false) :
		address(addr)
	{		
		debugL("SocketClientEchoTest") << "Creating: " << addr << endl;

		socket.Recv += delegate(this, &SocketClientEchoTest::onRecv);
		socket.Connect += delegate(this, &SocketClientEchoTest::onConnect);
		socket.Error += delegate(this, &SocketClientEchoTest::onError);
		socket.Close += delegate(this, &SocketClientEchoTest::onClose);
	}

	~SocketClientEchoTest()
	{
		debugL("SocketClientEchoTest") << "Destroying" << endl;

		assert(socket.base().refCount() == 1);
	}

	void start() 
	{
		// Create the socket instance on the stack.
		// When the socket is closed it will unref the main loop
		// causing the test to complete successfully.
		socket.connect(address);
		assert(socket.base().refCount() == 1);
	}

	void stop() 
	{
		//socket.close();
		socket.shutdown();
	}
	
	void onConnect(void* sender)
	{
		debugL("SocketClientEchoTest") << "Connected" << endl;
		assert(sender == &socket);
		socket.send("client > server", 15, WebSocket::FRAME_TEXT);
	}
	
	void onRecv(void* sender, net::SocketPacket& packet)
	{
		assert(sender == &socket);
		string data(packet.data(), 15);
		debugL("SocketClientEchoTest") << "Recv: " << data << endl;	

		// Check for return packet echoing sent data
		if (data == "client > server") {
			debugL("SocketClientEchoTest") << "Recv: Got Return Packet!" << endl;
			
			// Send the shutdown command to close the connection gracefully.
			// The peer disconnection will trigger an error callback which
			// will result is socket closure.
			socket.base().shutdown();
		}
		else
			assert(0 && "not echo response"); // fail...
	}

	void onError(void* sender, const Error& err)
	{
		errorL("SocketClientEchoTest") << "On error: " << err.message << endl;
		assert(sender == &socket);
	}
	
	void onClose(void* sender)
	{
		debugL("SocketClientEchoTest") << "On close" << endl;
		assert(sender == &socket);
	}
};


// -------------------------------------------------------------------
//
class Tests
{
public:
	Application app; 	

	Tests()
	{	
		debugL("Tests") << "#################### Starting" << endl;
#ifdef _MSC_VER
		_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
#if TEST_SSL
			// Init SSL Context 
			SSLContext::Ptr ptrContext = new SSLContext(
				SSLContext::CLIENT_USE, "", "", "", 
				SSLContext::VERIFY_NONE, 9, false, 
				"ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");		
#endif
		{		
			
			runURLTests();

			/*
			runClientConnectionChunkedTest();	
			runClientConnectionTest();
			runStandaloneHTTPClientConnectionTest();	
			runHTTPClientTest();	
			runWebSocketClientServerTest();
			runWebSocketSocketTest();
			runHTTPClientWebSocketTest();	
			runWebSocketSecureClientConnectionTest();
			runWebSocketClientConnectionTest();	
			*/

			// NOTE: Must be terminated with Crtl-C
			//runHTTPServerTest();
		}
#if TEST_SSL
			// Shutdown SSL
			SSLManager::instance().shutdown();
#endif

		// Shutdown the garbage collector so we can free memory.
		GarbageCollector::instance().shutdown();
		
		// Run the final cleanup
		//runCleanup();
		
		debugL("Tests") << "#################### Finalizing" << endl;
		app.cleanup();
		debugL("Tests") << "#################### Exiting" << endl;
	}

	void runLoop() {
		debugL("Tests") << "#################### Running" << endl;
		app.run();
		debugL("Tests") << "#################### Ended" << endl;
	}
	
	// ============================================================================
	// HTTP URL Tests
	//
	void runURLTests()
	{	
		http::URL url;
		assert (url.scheme().empty());
		assert (url.authority().empty());
		assert (url.userInfo().empty());
		assert (url.host().empty());
		assert (url.port() == 0);
		assert (url.path().empty());
		assert (url.query().empty());
		assert (url.fragment().empty());

		URL url2("HTTP", "localhost", "/home/sourcey/foo.bar");
		assert (url2.scheme() == "http");
		assert (url2.host() == "localhost");
		assert (url2.path() == "/home/sourcey/foo.bar");
	
		URL url3("http", "www.sourcey.com", "/index.html");
		assert (url3.scheme() == "http");
		assert (url3.authority() == "www.sourcey.com");
		assert (url3.host() == "www.sourcey.com");
		assert (url3.path() == "/index.html");
	
		URL url4("http", "www.sourcey.com:8000", "/index.html");
		assert (url4.scheme() == "http");
		assert (url4.authority() == "www.sourcey.com:8000");
		assert (url4.host() == "www.sourcey.com");
		assert (url4.path() == "/index.html");

		URL url5("http", "user@www.sourcey.com:8000", "/index.html");
		assert (url5.scheme() == "http");
		assert (url5.userInfo() == "user");
		assert (url5.host() == "www.sourcey.com");
		assert (url5.port() == 8000);
		assert (url5.authority() == "user@www.sourcey.com:8000");
		assert (url5.path() == "/index.html");

		URL url6("http", "user@www.sourcey.com:80", "/index.html");
		assert (url6.scheme() == "http");
		assert (url6.userInfo() == "user");
		assert (url6.host() == "www.sourcey.com");
		assert (url6.port() == 80);
		assert (url6.authority() == "user@www.sourcey.com:80");
		assert (url6.path() == "/index.html");

		URL url7("http", "www.sourcey.com", "/index.html", "query=test", "fragment");
		assert (url7.scheme() == "http");
		assert (url7.authority() == "www.sourcey.com");
		assert (url7.path() == "/index.html");
		assert (url7.pathEtc() == "/index.html?query=test#fragment");
		assert (url7.query() == "query=test");
		assert (url7.fragment() == "fragment");

		URL url8("http", "www.sourcey.com", "/index.html?query=test#fragment");
		assert (url8.scheme() == "http");
		assert (url8.authority() == "www.sourcey.com");
		assert (url8.path() == "/index.html");
		assert (url8.pathEtc() == "/index.html?query=test#fragment");
		assert (url8.query() == "query=test");
		assert (url8.fragment() == "fragment");
	}

	
	// ============================================================================
	// HTTP Client Tests
	//
	struct HTTPClientTest 
	{

		//*transaction->Receiver += delegate(this, &Tests::onTransactionComplete);


		http::Server server;
		http::Client client;
		int numSuccess;
		ClientConnection* conn;
		RandomDataSource dataSource;		

		HTTPClientTest() :
			numSuccess(0),
			conn(0),
			server(TEST_HTTP_PORT, new OurServerResponderFactory) 
		{
		}
		
		template<class ConnectionT>
		ConnectionT* create(bool raiseServer = true, const std::string& host = "127.0.0.1", UInt16 port = TEST_HTTP_PORT)
		{
			if (raiseServer)
				server.start();

			/*
			conn = (ClientConnection*)client.createConnectionT<ConnectionT>(host, port);		
			conn->Connect += delegate(this, &HTTPClientTest::onConnect);	
			conn->Headers += delegate(this, &HTTPClientTest::onHeaders);
			//conn->Receiver += delegate(this, &HTTPClientTest::onInboundStream);
			conn->Payload += delegate(this, &HTTPClientTest::onPayload);
			conn->Complete += delegate(this, &HTTPClientTest::onComplete);
			conn->Close += delegate(this, &HTTPClientTest::onClose);
			return (ConnectionT*)conn;
			*/
			assert(0);
		}

		void shutdown()
		{
			// Stop the client and server to release the loop
			server.shutdown();
			client.shutdown();
		}
			
		void onConnect(void*)
		{
			debugL("ClientConnectionTest") << "On connect" <<  endl;
			
			// Bounce backwards and forwards a few times :)
			//conn->write("BOUNCE", 6);

			// Start the output stream when the socket connects.
			//dataSource.conn = this->conn;
			//dataSource.start();
		}

		void onHeaders(void*, Response& res)
		{
			debugL("ClientConnectionTest") << "On headers" <<  endl;
			
			// Bounce backwards and forwards a few times :)
			//conn->write("BOUNCE", 6);
		}
		
		void onPayload(void*, Buffer& buf)
		{
			debugL("ClientConnectionTest") << "On response payload: " << buf << endl;

			if (buf.toString() == "BOUNCE")
				numSuccess++;
			
			debugL("ClientConnectionTest") << "On response payload: " << buf << ": " << numSuccess << endl;
			if (numSuccess >= 100) {
				
				debugL("ClientConnectionTest") << "SUCCESS: " << numSuccess << endl;
				conn->close();
			}
			else
				conn->write("BOUNCE", 6);
		}

		void onComplete(void*, const Response& res)
		{		
			ostringstream os;
			res.write(os);
			debugL("ClientConnectionTest") << "Response complete: " << os.str() << endl;
		}

		void onClose(void*)
		{	
			debugL("ClientConnectionTest") << "Connection closed" << endl;
			shutdown();
		}
	};

	void runClientConnectionTest() 
	{	
		HTTPClientTest test;
		test.create<ClientConnection>()->send(); // default GET request
		runLoop();
	}

	void runClientConnectionChunkedTest() 
	{	
		HTTPClientTest test;		
		ClientConnection* conn = test.create<ClientConnection>(false, "127.0.0.1", TEST_HTTP_PORT);
		conn->request().setKeepAlive(true);
		conn->request().setURI("/chunked");
		//conn->request().body << "BOUNCE" << endl;
		conn->send();
		runLoop();
	}
	
	void runWebSocketClientConnectionTest() 
	{	
		HTTPClientTest test;
		ClientConnection* conn = test.create<WebSocketClientConnection>(false, "127.0.0.1", TEST_HTTP_PORT);
		conn->shouldSendHead(false);
		conn->request().setURI("/websocket");
		//conn->request().body << "BOUNCE" << endl;
		conn->send();
		runLoop();
	}

	void runWebSocketSecureClientConnectionTest() 
	{	
		HTTPClientTest test;
		ClientConnection* conn = test.create<WebSocketSecureClientConnection>(false, "127.0.0.1", TEST_HTTPS_PORT);
		conn->request().setURI("/websocket");
		//conn->request().body << "BOUNCE" << endl;
		conn->send();
		runLoop();
	}
	
	
	// ============================================================================
	// Standalone HTTP Client ConnectionTest
	//
	void runStandaloneHTTPClientConnectionTest()
	{
		ClientConnection* conn = new ClientConnection("google.com");
		conn->Headers += delegate(this, &Tests::onStandaloneHTTPClientConnectionHeaders);
		conn->Payload += delegate(this, &Tests::onStandaloneHTTPClientConnectionPayload);
		conn->Complete += delegate(this, &Tests::onStandaloneHTTPClientConnectionComplete);
		conn->send(); // send default GET /
		runLoop();
	}	
	
	void onStandaloneHTTPClientConnectionHeaders(void*, Response& res)
	{	
		debugL("StandaloneClientConnectionTest") << "On response headers: " << res << endl;
	}
	
	void onStandaloneHTTPClientConnectionPayload(void*, Buffer& buf)
	{	
		debugL("StandaloneClientConnectionTest") << "On response payload: " << buf << endl;
	}
	
	void onStandaloneHTTPClientConnectionComplete(void* sender, const Response& res)
	{		
		debugL("StandaloneClientConnectionTest") << "On response complete" << endl;
		auto self = reinterpret_cast<ClientConnection*>(sender);
		self->close();
	}

	
	
	// ============================================================================
	// WebSocket Test
	//
	void runClientWebSocketTest() 
	{
		//http::Server srv(TEST_HTTP_PORT, new OurServerResponderFactory);
		//srv.start();

		// ws://echo.websocket.org		
		//SocketClientEchoTest<http::WebSocket> test(net::Address("174.129.224.73", 1339));
		//SocketClientEchoTest<http::WebSocket> test(net::Address("174.129.224.73", 80));

		//debugL("Tests") << "TCP Socket Test: Starting" << endl;
		//SocketClientEchoTest<http::WebSocket> test(net::Address("127.0.0.1", TEST_HTTP_PORT));
		//test.start();

		//runLoop();
	}	

	/*
	// ============================================================================
	// WebSocket Test
	//
	void runWebSocketServerTest() 
	{
		http::Server srv(TEST_HTTP_PORT, new OurServerResponderFactory);
		srv.start();

		debugL("Tests") << "TCP Socket Test: Starting" << endl;			
		// ws://echo.websocket.org		
		//SocketClientEchoTest<http::WebSocket> test(net::Address("174.129.224.73", 1339));
		//SocketClientEchoTest<http::WebSocket> test(net::Address("174.129.224.73", 80));
		//SocketClientEchoTest<http::WebSocket> test(net::Address("127.0.0.1", 1340));
		//test.start();

		runLoop();
	}
	*/
	
	// ============================================================================
	// HTTP Server Test
	//
	void runHTTPServerTest() 
	{
		http::Server srv(TEST_HTTP_PORT, new OurServerResponderFactory);
		srv.start();
		
		// Catch close signal to shutdown the server
		// This should free the main loop.
		uv_signal_t* sig = new uv_signal_t;
		sig->data = &srv;
		uv_signal_init(&app.loop, sig);
		uv_signal_start(sig, Tests::onKillHTTPServer, SIGINT);

		runLoop();
	}
	
	static void onPrintHTTPServerHandle(uv_handle_t* handle, void* arg) 
	{
		debugL("HTTPServerTest") << "#### Active HTTPServer Handle: " << handle << endl;
	}

	static void onKillHTTPServer(uv_signal_t *req, int signum)
	{
		debugL("HTTPServerTest") << "Kill Signal: " << req << endl;
	
		// print active handles
		uv_walk(req->loop, Tests::onPrintHTTPServerHandle, NULL);
	
		((http::Server*)req->data)->shutdown();

		uv_signal_stop(req);
	}

};


} } // namespace scy::http


int main(int argc, char** argv) 
{	
	Logger::instance().add(new ConsoleChannel("debug", TraceLevel));
	{
		http::Tests app;
	}
	Logger::uninitialize();
	return 0;
}




		/*
		uv_signal_t sig;
		sig.data = &srv;
		uv_signal_init(app.loop, &sig);
		uv_signal_start(&sig, Tests::onKillHTTPServer, SIGINT);
		*/
			
			/*

			(sock);
			net::TCPSocket sock;	
			runHTTPClientTest();

			uv_signal_t sig;
			sig.data = this;
			uv_signal_init(app.loop, &sig);
			uv_signal_start(&sig, Tests::onKillSignal2, SIGINT);

			runUDPSocketTest();
			runTimerTest();
			//runDNSResolverTest();
			
			debugL("Tests") << "#################### Running" << endl;
			app.run();
			debugL("Tests") << "#################### Ended" << endl;
			*/

			/*		
			http::Server srv(81, new OurServerResponderFactory);
			srv.start();

			//runHTTPClientTest();	
			(sock);
			net::TCPSocket sock;	
			runHTTPClientTest();

			uv_signal_t sig;
			sig.data = this;
			uv_signal_init(app.loop, &sig);
			uv_signal_start(&sig, Tests::onKillSignal2, SIGINT);

			runUDPSocketTest();
			runTimerTest();
			//runDNSResolverTest();
			
			debugL("Tests") << "#################### Running" << endl;
			app.run();
			debugL("Tests") << "#################### Ended" << endl;
			*/
	
		/*
	// ============================================================================
	// HTTP ClientConnection Test
	//
	void runHTTPClientTest() 
	{
		debugL("ClientConnectionTest") << "Starting" << endl;	

		// Setup the transaction
		http::Request req("GET", "http://google.com");
		http::Response res;
		http::ClientConnection txn(&req);
		txn.Complete += delegate(this, &Tests::onComplete);
		txn.DownloadProgress += delegate(this, &Tests::onIncomingProgress);	
		txn.send();

		// Run the looop
		app.run();
		//util::pause();

		debugL("ClientConnectionTest") << "Ending" << endl;
	}		

	void onComplete(void* sender, http::Response& response)
	{
		debugL("ClientConnectionTest") << "On Complete: " << &response << endl;
	}

	void onIncomingProgress(void* sender, http::TransferProgress& progress)
	{
		debugL("ClientConnectionTest") << "On Progress: " << progress.progress() << endl;
	}
		*/
	
	
//using http::Request;
//using http::Response;
//using http::ClientConnection;

/*
struct Result {
	int numSuccess;
	string name;
	Stopwatch sw;

	void reset() {
		numSuccess = 0;
		sw.reset();
	}
};

static Result Benchmark;
*/