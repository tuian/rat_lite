#include "ClientNetworkDriver.h"
#include "turbojpeg.h"
#include "IClientDriver.h"
#include "WebSocket.h"
#include "Shapes.h"
#include "Image.h"
#include "Input.h"
#include "Logging.h"
#include "Configs.h"
#include "IWebSocket.h"
#include "internal/WebSocket.h"
#include "uWS.h"
#include <assert.h>


namespace SL {
	namespace RAT {

		class ClientNetworkDriverImpl {

			IClientDriver* IClientDriver_;
			std::shared_ptr<Client_Config> Config_;
			uWS::Hub h;
			Point LastMousePosition_;
			std::shared_ptr<IWebSocket> Socket_;
			std::thread Runner;
			void MouseImage(const IWebSocket& socket, const char* data, size_t len) {
				assert(len >= sizeof(Rect));
				Image img(*reinterpret_cast<const Rect*>(data), data + sizeof(Rect), len - sizeof(Rect));
				assert(len >= sizeof(Rect) + (img.Rect.Width * img.Rect.Height * PixelStride));
				IClientDriver_->onReceive_MouseImage(img);
			}

			void MousePos(const IWebSocket& socket, const char* data, size_t len) {
				assert(len == sizeof(Point));
				IClientDriver_->onReceive_MousePos(reinterpret_cast<const Point*>(data));
			}
			void ScreenImage(const IWebSocket& socket, const char* data, size_t len, bool dif) {
				assert(len >= sizeof(Rect));

				thread_local auto jpegDecompressor = tjInitDecompress();
				thread_local std::vector<char> outputbuffer;

				int jpegSubsamp(0), outwidth(0), outheight(0);
				auto src = (unsigned char*)(data + sizeof(Rect));

				if (tjDecompressHeader2(jpegDecompressor, src, static_cast<unsigned long>(len - sizeof(Rect)), &outwidth, &outheight, &jpegSubsamp) == -1) {
					SL_RAT_LOG(Logging_Levels::ERROR_log_level, tjGetErrorStr());
				}
				outputbuffer.reserve(outwidth* outheight * PixelStride);

				if (tjDecompress2(jpegDecompressor, src, static_cast<unsigned long>(len - sizeof(Rect)), (unsigned char*)outputbuffer.data(), outwidth, 0, outheight, TJPF_RGBX, TJFLAG_FASTDCT | TJFLAG_NOREALLOC) == -1) {
					SL_RAT_LOG(Logging_Levels::ERROR_log_level, tjGetErrorStr());
				}
				Image img(*reinterpret_cast<const Rect*>(data), outputbuffer.data(), outwidth* outheight * PixelStride );
				assert(outwidth == img.Rect.Width && outheight == img.Rect.Height);
				if (dif) {
					IClientDriver_->onReceive_ImageDif(img);
				}
				else {
					IClientDriver_->onReceive_Image(img);
				}
			}


		public:
			ClientNetworkDriverImpl(IClientDriver* r) :
				IClientDriver_(r) {

			}
			void Connect(std::shared_ptr<Client_Config> config, const char* dst_host) {
				Config_ = config;
				auto hc = std::string("ws://") + std::string(dst_host) + std::string(":") + std::to_string(config->WebSocketTLSLPort);
				h.connect(hc, nullptr);
				h.onConnection([&](uWS::WebSocket<uWS::CLIENT> ws, uWS::HttpRequest req) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "onConnection ");
					IClientDriver_->onConnection(std::make_shared<WebSocket<uWS::WebSocket<uWS::CLIENT>>>(ws));
				});
				h.onDisconnection([&](uWS::WebSocket<uWS::CLIENT> ws, int code, char *message, size_t length) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "onDisconnection ");
					WebSocket<uWS::WebSocket<uWS::CLIENT>> sock(ws);
					IClientDriver_->onDisconnection(sock, code, message, length);
				});
				h.onMessage([&](uWS::WebSocket<uWS::CLIENT> ws, char *message, size_t length, uWS::OpCode code) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "onMessage ");
					auto p = *reinterpret_cast<const PACKET_TYPES*>(message);
					WebSocket<uWS::WebSocket<uWS::CLIENT>> sock(ws);
					switch (p) {
					case PACKET_TYPES::SCREENIMAGE:
						ScreenImage(sock, message + sizeof(p), length - sizeof(p), false);
						break;
					case PACKET_TYPES::SCREENIMAGEDIF:
						ScreenImage(sock, message + sizeof(p), length - sizeof(p), true);
						break;
					case PACKET_TYPES::MOUSEIMAGE:
						MouseImage(sock, message + sizeof(p), length - sizeof(p));
						break;
					case PACKET_TYPES::MOUSEPOS:
						MousePos(sock, message + sizeof(p), length - sizeof(p));
						break;
					case PACKET_TYPES::CLIPBOARDTEXTEVENT:
						IClientDriver_->onReceive_ClipboardText(message + sizeof(p), length - sizeof(p));
						break;
					default:
						IClientDriver_->onMessage(sock, message, length);//pass up the chain
						break;
					}
				});

				Runner = std::thread([&]() { h.run(); });
			}

			virtual ~ClientNetworkDriverImpl() {
				if (Runner.joinable()) {
					Runner.join();
				}

			}


			void SendMouse(const MouseEvent& m) {
				if (!Socket_) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "SendMouse called on a socket that is not open yet");
					return;
				}
				if (Socket_->is_loopback()) return;//dont send mouse info to ourselfs as this will cause a loop
				//do checks to prevent sending redundant mouse information about its position
				if (m.EventData == NO_EVENTDATA && LastMousePosition_ == m.Pos && m.PressData == NO_PRESS_DATA && m.ScrollDelta == 0) {
					return;//already did this event
				}
				LastMousePosition_ = m.Pos;
				auto ptype = PACKET_TYPES::MOUSEEVENT;
				const auto size = sizeof(ptype) + sizeof(m);
				char ptr[size];
				*reinterpret_cast<PACKET_TYPES*>(ptr) = ptype;
				memcpy(ptr + sizeof(ptype), &m, sizeof(m));
				Socket_->send(ptr, size);
			}
			void SendKey(const KeyEvent & m) {
				if (!Socket_) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "SendKey called on a socket that is not open yet");
					return;
				}
				auto ptype = PACKET_TYPES::KEYEVENT;
				const auto size = sizeof(ptype) + sizeof(m);
				char ptr[size];
				*reinterpret_cast<PACKET_TYPES*>(ptr) = ptype;
				memcpy(ptr + sizeof(ptype), &m, sizeof(m));
				Socket_->send(ptr, size);

			}
			void SendClipboardText(const char* data, unsigned int len) {
				if (!Socket_) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "SendClipboardText called on a socket that is not open yet");
					return;
				}
				if (Socket_->is_loopback()) return;//dont send clipboard info to ourselfs as it will cause a loop

				auto ptype = PACKET_TYPES::CLIPBOARDTEXTEVENT;
				auto size = sizeof(ptype) + len;
				auto ptr(std::make_unique<char[]>(size));
				*reinterpret_cast<PACKET_TYPES*>(ptr.get()) = ptype;
				memcpy(ptr.get() + sizeof(ptype), data, len);
				Socket_->send(ptr.get(), size);

			}
			std::shared_ptr<IWebSocket> getSocket()const {
				return Socket_;
			}
		};

	}
}

SL::RAT::ClientNetworkDriver::ClientNetworkDriver(IClientDriver * r)
	: _ClientNetworkDriverImpl(new ClientNetworkDriverImpl(r))
{

}

SL::RAT::ClientNetworkDriver::~ClientNetworkDriver()
{
	delete _ClientNetworkDriverImpl;
}

void SL::RAT::ClientNetworkDriver::Connect(std::shared_ptr<Client_Config> config, const char* dst_host)
{
	_ClientNetworkDriverImpl->Connect(config, dst_host);
}


void SL::RAT::ClientNetworkDriver::SendKey(const KeyEvent & m)
{
	_ClientNetworkDriverImpl->SendKey(m);
}

void SL::RAT::ClientNetworkDriver::SendMouse(const MouseEvent& m)
{
	_ClientNetworkDriverImpl->SendMouse(m);
}


void SL::RAT::ClientNetworkDriver::SendClipboardText(const char* data, unsigned int len) {
	return _ClientNetworkDriverImpl->SendClipboardText(data, len);
}