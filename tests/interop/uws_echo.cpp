#include <cstdlib>
#include "App.h"

struct PerSocketData { };

int main(int argc, char **argv) {
	int port = argc > 1 ? std::atoi(argv[1]) : 9001;

	uWS::App()
	    .ws<PerSocketData>(
	        "/*",
	        {.message = [](auto *ws, std::string_view message,
	                       uWS::OpCode opCode) { ws->send(message, opCode); }})
	    .listen(port,
	            [port](auto *listen_socket) {
		            if (listen_socket)
			            std::cout << "uWS echo on " << port << std::endl;
	            })
	    .run();
}
