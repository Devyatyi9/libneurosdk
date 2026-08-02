#include <cstdlib>
#include "App.h"

struct PerSocketData { };

int main(int argc, char **argv) {
	int port = argc > 1 ? std::atoi(argv[1]) : 9001;

	uWS::App()
	    .ws<PerSocketData>(
	        "/*",
	        {.compression = uWS::CompressOptions(uWS::DEDICATED_COMPRESSOR |
	                                             uWS::DEDICATED_DECOMPRESSOR),
	         .message =
	             [](auto *ws, std::string_view message, uWS::OpCode opCode) {
		             if (!ws->hasNegotiatedCompression()) {
			             std::cerr << "permessage-deflate was not negotiated"
			                       << std::endl;
			             ws->close();
			             return;
		             }
		             ws->send(message, opCode, true);
	             }})
	    .listen(port,
	            [port](auto *listen_socket) {
		            if (listen_socket)
			            std::cout << "uWS echo on " << port << std::endl;
	            })
	    .run();
}
