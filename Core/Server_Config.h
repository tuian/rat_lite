#pragma once
#include <string>

namespace SL {
	namespace Remote_Access_Library {
		namespace Network {
			struct Server_Config {
				//both the viewer and web client communicate over web sockets.
				unsigned short WebSocketListenPort = 0;
				unsigned short WebSocketTLSListenPort = 0;
				unsigned int Read_Timeout = 5;//in seconds
				unsigned int Write_Timeout = 5;//in seconds
				unsigned short HttpListenPort = 0;
				unsigned short HttpTLSListenPort = 0;
				//this is where files are issued out for the webserver. If no path is specified, wwwroot in applications directory is chosen as a default
				std::string WWWRoot;
				
			};
		}
	}
}