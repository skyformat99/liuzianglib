#pragma once
#ifndef liuzianglib_IOCP
#define liuzianglib_IOCP
#include <stdlib.h>
#include <winsock2.h>
#include <MSWSock.h>
#include "liuzianglib.h"
#include "DC_WinSock.h"
#include "DC_ThreadPool.h"
#include "DC_type.h"
#include "DC_timer.h"
#include <vector>
#pragma comment(lib,"ws2_32.lib")
//Version 2.4.21V19
//20170724

//1.删掉不必要的部分
//2.重写所有代码
//3.使用方法不是继承，而是设置回调函数
//4.main函数使用getch阻塞，可以手动clear或者暂停/开始
//5.cleaner删除套接字资源前使用CancelIoEx清除IOCP内部队列中其上所有IO请求
//6.worker线程检测到客户端已经断开连接或者超时了的话会立刻关闭套接字，但是套接字的资源依然要等cleaner来回收

namespace DC {

	namespace Web {

		namespace IOCP {

			namespace IOCPSpace {

				class SpinLock final {
				public:
					SpinLock() :flag{ 0 } {}

					SpinLock(const SpinLock&) = delete;

				public:
					void lock() {
						while (flag.test_and_set(std::memory_order::memory_order_acquire));
					}

					bool try_lock() {
						return !flag.test_and_set(std::memory_order_acquire);
					}

					void unlock() {
						flag.clear(std::memory_order::memory_order_release);
					}

				private:
					std::atomic_flag flag;
				};

				enum OperationType { ACCEPT, SEND, RECV, EXIT, NOSET };

				template <typename _Ty>
				class IOCPAllocator final {//designed for DC::IOCP
				public:
					using value_type = _Ty;
					using pointer = _Ty*;
					using const_pointer = const pointer;
					using size_type = DC::size_t;

				private:
					using size_t = DC::size_t;

				public:
					IOCPAllocator(size_t size) :m_list(size) {}

					IOCPAllocator(const IOCPAllocator&) = delete;

					~IOCPAllocator()noexcept {
						this->clear();
					}

				public:
					template <typename ...ARGS>
					pointer make(ARGS&& ...args)noexcept {
						auto ptr = reinterpret_cast<pointer>(malloc(sizeof(value_type)));
						if (ptr == NULL)
							return nullptr;

						try {
							new(ptr) value_type(std::forward<ARGS>(args)...);
							std::lock_guard<std::mutex> lm_mut(m_mut);
							m_list.push_back(ptr);
						}
						catch (...) {
							free(ptr);
							ptr = nullptr;
						}
						return ptr;
					}

					template <typename ...ARGS>
					static pointer make_norecord(ARGS&& ...args)noexcept {
						auto ptr = reinterpret_cast<pointer>(malloc(sizeof(value_type)));
						if (ptr == NULL)
							return nullptr;

						try {
							new(ptr) value_type(std::forward<ARGS>(args)...);
						}
						catch (...) {
							free(ptr);
							ptr = nullptr;
						}
						return ptr;
					}

					inline void put(value_type& obj)noexcept {
						try {
							std::lock_guard<std::mutex> lm_mut(m_mut);
							m_list.push_back(&obj);
						}
						catch (...) {}
					}

					void remove(pointer ptr) {
						std::lock_guard<std::mutex> lm_mut(m_mut);

						auto fres = std::find_if(m_list.begin(), m_list.end(), [&ptr](const pointer& it) {
							return ptr == it;
						});
						if (fres == m_list.end()) return;

						destory(*fres);
						DC::vector_fast_erase_no_return(m_list, fres);
					}

					void clear()noexcept {
						m_list.reserve(0);
					}

					bool empty()const {
						return m_list.empty();
					}

					inline void destory(void* ptr) {
						if (isNull(ptr)) return;
						reinterpret_cast<pointer>(ptr)->~value_type();
						free(ptr);
					}

					std::vector<pointer>& get_list() {
						return m_list;
					}

					std::mutex& get_mut() {
						return m_mut;
					}

				private:
					std::vector<pointer> m_list;
					std::mutex m_mut;
				};

				struct IOContext {
					OVERLAPPED m_overlapped;
					OperationType m_type;
					WSABUF m_wsabuf;
					DC::WinSock::Socket m_socket;

					IOContext(const DC::size_t& _Buffer_Size) :m_overlapped{ 0 }, m_type(OperationType::NOSET), m_wsabuf{ 0 } {
						make_buffer(_Buffer_Size);
					}

					IOContext(const IOContext&) = delete;

					~IOContext() {
						if (!isNull(m_wsabuf.buf)) {
							free(m_wsabuf.buf);
							m_wsabuf.buf = nullptr;
							m_wsabuf.len = 0;
						}
					}

					void make_buffer(const DC::size_t& _Buffer_Size) {
						if (_Buffer_Size != 0) {
							m_wsabuf.buf = reinterpret_cast<decltype(WSABUF::buf)>(malloc(_Buffer_Size));
							m_wsabuf.len = _Buffer_Size;
							if (isNull(m_wsabuf.buf)) {
								m_wsabuf.buf = nullptr;
								m_wsabuf.len = 0;
								throw DC::Exception("IOContext::make_buffer", "malloc returned NULL");
							}
							reset_buffer();
						}
						else {
							m_wsabuf.buf = nullptr;
							m_wsabuf.len = 0;
						}
					}

					inline void reset_buffer() {
						if (!isNull(m_wsabuf.buf))
							memset(m_wsabuf.buf, 0, m_wsabuf.len);
					}

					inline void reset_overlapped() {
						memset(&m_overlapped, 0, sizeof(m_overlapped));
					}

					inline bool check_socket()const {
						return m_socket != INVALID_SOCKET;
					}
				};

				class SocketContext {
				public:
					SocketContext() :m_IOContextPool(0), m_socket(INVALID_SOCKET), m_removeRightnow(false) {
						memset(&m_clientAddress, 0, sizeof(m_clientAddress));
					}

					SocketContext(const SocketContext&) = delete;

					~SocketContext() {
						if (isNull(this)) return;
						close_socket();
						this->m_IOContextPool.clear();
					}

				public:
					void set_socket(const DC::WinSock::Socket& input) {
						m_socket = input;
					}

					DC::WinSock::Socket get_socket()const {
						return m_socket;
					}

					DC::WinSock::Address get_client_address()const {
						return m_clientAddress;
					}

					inline void set_client_address(const DC::WinSock::Address& input) {
						m_clientAddress = input;
					}

					void close_socket() {
						if (m_socket != INVALID_SOCKET) {
							std::lock_guard<SpinLock> lockit(m_socket_lock);
							DC::WinSock::Close(m_socket);
						}
					}

					DC::timer get_timer()const {
						return m_timer;
					}

					inline bool check_socket()const {
						return m_socket != INVALID_SOCKET;
					}

					inline void set_removeRightnow(bool _Val)noexcept {
						m_removeRightnow.store(std::memory_order::memory_order_release);
					}

					inline bool get_removeRightnow()const noexcept {
						return m_removeRightnow.load(std::memory_order::memory_order_acquire);
					}

				public:
					template <typename ...ARGS>
					inline IOContext* make_IOContext(ARGS&& ...args) {
						auto temp = m_IOContextPool.make(std::forward<ARGS>(args)...);
						temp->m_socket = this->m_socket;
						return temp;
					}

					void remove_IOContext(IOContext* ptr) {
						if (isNull(this)) return;
						m_IOContextPool.remove(ptr);
					}

					void clear_IOContext() {
						m_IOContextPool.clear();
					}

				private:
					std::atomic<bool> m_removeRightnow;
					DC::WinSock::Socket m_socket;
					DC::WinSock::Address m_clientAddress;
					DC::timer m_timer;
					SpinLock m_socket_lock;

					IOCPAllocator<IOContext> m_IOContextPool;
				};

				struct PSCdeleter {
				public:
					inline void operator()(SocketContext *ptr)const {
						ptr->~SocketContext();
						free(ptr);
					}
				};

				inline bool AssociateWithIOCP(HANDLE IOCP, SocketContext* Socketptr) {
					if (isNull(Socketptr)) return false;
					if (!Socketptr->check_socket()) return false;
					return CreateIoCompletionPort(reinterpret_cast<HANDLE>(Socketptr->get_socket()), IOCP, reinterpret_cast<ULONG_PTR>(Socketptr), 0) != NULL;
				}

				bool PostAccept(const SocketContext& ListenSocket, IOContext* IOptr, LPFN_ACCEPTEX AcceptExFunc)noexcept {
					if (isNull(IOptr)) return false;
					if (!IOptr->check_socket()) return false;

					DWORD dwBytes = 0;
					IOptr->m_type = OperationType::ACCEPT;
					IOptr->reset_buffer();

					DC::WinSock::SocketInitOverlapped(IOptr->m_socket);
					if (!IOptr->check_socket()) {
						DC::WinSock::Close(IOptr->m_socket);
						return false;
					}

					if (FALSE == AcceptExFunc(ListenSocket.get_socket(), IOptr->m_socket, IOptr->m_wsabuf.buf, 0, sizeof(DC::WinSock::Address) + 16, sizeof(DC::WinSock::Address) + 16, &dwBytes, &IOptr->m_overlapped))
						if (WSA_IO_PENDING != WSAGetLastError())
							return false;

					return true;
				}

				bool PostRecv(IOContext* IOptr)noexcept {
					if (isNull(IOptr)) return false;
					if (!IOptr->check_socket()) return false;

					DWORD dwBytes = 0, dwFlags = 0;
					IOptr->m_type = OperationType::RECV;
					IOptr->reset_buffer();

					if (SOCKET_ERROR == WSARecv(IOptr->m_socket, &IOptr->m_wsabuf, 1, &dwBytes, &dwFlags, &IOptr->m_overlapped, 0)) {
						if (WSA_IO_PENDING != WSAGetLastError())
							return false;
					}

					return true;
				}

				bool PostSend(SocketContext* Socketptr, const std::string& sendstr)noexcept {
					if (isNull(Socketptr)) return false;
					if (!Socketptr->check_socket()) return false;

					DWORD dwBytes = sendstr.size();

					auto IOptr = Socketptr->make_IOContext(sendstr.size());
					if (isNull(IOptr))
						return false;

					memcpy(IOptr->m_wsabuf.buf, sendstr.c_str(), sendstr.size());
					IOptr->m_type = OperationType::SEND;

					if (SOCKET_ERROR == WSASend(Socketptr->get_socket(), &IOptr->m_wsabuf, 1, &dwBytes, 0, &IOptr->m_overlapped, 0))
						if (WSA_IO_PENDING != WSAGetLastError())
							return false;

					return true;
				}

				bool PostExit(HANDLE IOCP, IOContext* IOptr)noexcept {
					if (isNull(IOptr)) return false;

					IOptr->m_type = OperationType::EXIT;

					if (0 != PostQueuedCompletionStatus(IOCP, 0, reinterpret_cast<ULONG_PTR>(IOptr), &IOptr->m_overlapped)) return true;
					return false;
				}

			}

			using reply_type = std::function<bool(const std::string&)>;

			template <typename OnAcceptCallbackType, typename OnRecvCallbackType, typename OnSendCallbackType, typename OnExceptCallbackType>
			class Server final {
			public:
				Server(const DC::size_t& worker_threadnumber, const DC::size_t& usercode_threadnumber, const std::string& listenip, const DC::size_t& listenport,
					const OnAcceptCallbackType& onacceptcallback,
					const OnRecvCallbackType& onrecvcallback,
					const OnSendCallbackType& onsendcallback,
					const OnExceptCallbackType& onexceptcallback) :
					m_iocp(nullptr), m_pscPool(0), m_io_tp(nullptr), m_usercode_tp(nullptr),
					m_io_tp_threadnumber(worker_threadnumber + 1), m_usercode_tp_threadnumber(usercode_threadnumber),
					m_listenAddress(DC::WinSock::MakeAddr(listenip, listenport)),
					m_recvbuffer_length(1024), m_client_alive_time(15000), m_cleaner_sleep_time(4000), m_cleaner_max_block_time(2000),
					m_onAcceptCallback(onacceptcallback),
					m_onRecvCallback(onrecvcallback),
					m_onSendCallback(onsendcallback),
					m_onExceptCallback(onexceptcallback) {
					this->get_wsa_extension_function();
				}

				Server(const Server&) = delete;

				Server& operator=(const Server&) = delete;

				~Server() {
					stop();
				}

			public:
				bool start(const DC::size_t& _Post_Accept_Number) {
					stop();

					start_tp();
					if (!check_tp()) 
						return false;
					
					m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
					if (isNull(m_iocp)) {
						stop_tp();
						m_iocp = nullptr;
						return false;
					}

					///*
					if (!start_listen(_Post_Accept_Number)) {
						stop_tp();
						CloseHandle(m_iocp);
						m_iocp = nullptr;
						m_pscPool.clear();
						return false;
					}
					//*/

					for (auto i = 0; i < m_io_tp_threadnumber - 1; i++)
						m_io_tp->async(&Server::worker, this);

					stopFlag.store(false, std::memory_order::memory_order_release);
					m_io_tp->async(&Server::cleaner, this);

					m_io_tp->start();
					m_usercode_tp->start();

					return true;
				}

				void stop() {
					stop_listen();
					
					IOCPSpace::IOCPAllocator<IOCPSpace::IOContext> ExitIO(m_io_tp_threadnumber);
					for (auto i = 0; i < m_io_tp_threadnumber; i++)
						IOCPSpace::PostExit(m_iocp, ExitIO.make(0));

					if (!isNull(m_iocp)) {
						CloseHandle(m_iocp);
						m_iocp = nullptr;
					}

					stopFlag.store(true, std::memory_order::memory_order_release);
					this->cleanerCV.notify_one();
					stop_tp();

					m_pscPool.clear();
					m_listenSocket.clear_IOContext();
				}

			public:
				inline void set_client_alive_time(const DC::size_t& _Time) {
					m_client_alive_time = _Time;
				}

				inline void set_cleaner_sleep_time(const std::chrono::milliseconds& _Time) {
					m_cleaner_sleep_time = _Time;
				}

				inline void set_cleaner_max_block_time(const DC::size_t& _Time) {
					m_cleaner_max_block_time = _Time;
				}

				inline void set_recv_buffer_size(const DC::size_t& _Size) {
					m_recvbuffer_length = _Size;
				}

			private:
				void worker()noexcept {
					DWORD dwBytesTransfered = 0;
					OVERLAPPED *pOverlapped = nullptr;
					IOCPSpace::SocketContext *PSC = nullptr;

					while (true) {
						BOOL GQrv = GetQueuedCompletionStatus(m_iocp, &dwBytesTransfered,
							reinterpret_cast<PULONG_PTR>(&PSC), &pOverlapped, INFINITE);

						//PSC为空(一般意味着完成端口已关闭)
						if (isNull(PSC)) break;
						//完成端口已关闭
						if (isNull(m_iocp)) break;
						//遇到了错误
						if (!GQrv) continue;

						IOCPSpace::IOContext* PIC = CONTAINING_RECORD(pOverlapped, IOCPSpace::IOContext, m_overlapped);

						//客户端已经断开连接
						if (dwBytesTransfered == 0 && (PIC->m_type == IOCPSpace::OperationType::SEND || PIC->m_type == IOCPSpace::OperationType::RECV)) {
							PSC->close_socket();
							continue;
						}
						switch (PIC->m_type) {
						case IOCPSpace::OperationType::RECV: {
							do_recv(PSC, PIC, dwBytesTransfered);
						}break;
						case IOCPSpace::OperationType::SEND: {
							do_send(PSC, PIC, dwBytesTransfered);
						}break;
						case IOCPSpace::OperationType::ACCEPT: {
							do_accept(PIC, this->m_GetAcceptExSockAddrs);
						}break;
						case IOCPSpace::OperationType::EXIT: {
							return;
						}break;
						case IOCPSpace::OperationType::NOSET: {
						}break;
						}
						//检查是否超时，是则关闭连接
						if (!isNull(PSC)) {
							if (PSC->get_timer().getms() >= m_client_alive_time) {
								PSC->set_removeRightnow(true);
								PSC->close_socket();
							}
						}
					}
				}

				void cleaner() {
					DC::size_t& block_time = m_cleaner_max_block_time, client_alive_time = m_client_alive_time;
					DC::timer timer;
					std::chrono::milliseconds templimits = m_cleaner_sleep_time;

					while (true) {
						timer.reset();
						timer.start();
						std::unique_lock<std::mutex> lock(cleanerMut);
						while (true) {
							if (cleanerCV.wait_for(lock, templimits) == std::cv_status::timeout)
								if (stopFlag.load(std::memory_order_acquire) == true)
									return;//退出信号
								else
									break;//时间到，开始工作

							timer.stop();

							if (stopFlag.load(std::memory_order_acquire) == true)
								return;//退出信号

							if (std::chrono::milliseconds(timer.getms()) >= templimits)
								break;//睡过头了

									  //假唤醒，计算剩余时间开始新一轮等待
							templimits = templimits - std::chrono::milliseconds(timer.getms());
							timer.reset();
							timer.start();
						}

						std::unique_lock<std::mutex> allocator_lock(m_pscPool.get_mut());
						//debug
						std::cout << "cleaner!\n";
						timer.reset();
						timer.start();
						for (auto it(m_pscPool.get_list().begin()); it != m_pscPool.get_list().end(); it++) {
							if (timer.getms() >= m_cleaner_max_block_time) break;
							if ((*it)->get_timer().getms() >= m_client_alive_time) {
								m_pscPool.remove(*it);
								continue;
							}
						}
					}
				}

			private:
				inline void get_wsa_extension_function() {
					DC::WinSock::Socket tempsock = INVALID_SOCKET;
					GUID GuidAcceptEx = WSAID_ACCEPTEX;
					GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
					DWORD dwBytes = 0;

					DC::WinSock::SocketInitTCP(tempsock);

					if (SOCKET_ERROR == WSAIoctl(tempsock, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &this->m_AcceptEx, sizeof(this->m_AcceptEx), &dwBytes, NULL, NULL)) {
						DC::WinSock::Close(tempsock);
						throw DC::Exception("dc_iocp_server::get_function", "can not get the pointer of function AcceptEx");
					}

					if (SOCKET_ERROR == WSAIoctl(tempsock, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidGetAcceptExSockAddrs, sizeof(GuidGetAcceptExSockAddrs), &this->m_GetAcceptExSockAddrs, sizeof(this->m_GetAcceptExSockAddrs), &dwBytes, NULL, NULL)) {
						DC::WinSock::Close(tempsock);
						throw DC::Exception("dc_iocp_server::get_function", "can not get the pointer of function GetAcceptExSockAddrs");
					}
				}

				bool start_listen(const DC::size_t& _Post_Size)noexcept {
					if (m_listenSocket.get_socket() != INVALID_SOCKET)
						return false;

					DC::WinSock::Socket listensocket = INVALID_SOCKET;
					DC::WinSock::SocketInitOverlapped(listensocket);
					if (listensocket == INVALID_SOCKET)
						return false;
					m_listenSocket.set_socket(listensocket);

					if (!IOCPSpace::AssociateWithIOCP(reinterpret_cast<HANDLE>(this->m_iocp), &m_listenSocket)) {
						m_listenSocket.close_socket();
						m_listenSocket.set_socket(INVALID_SOCKET);
						return false;
					}

					if (!DC::WinSock::Bind(m_listenSocket.get_socket(), this->m_listenAddress)) {
						m_listenSocket.close_socket();
						m_listenSocket.set_socket(INVALID_SOCKET);
						return false;
					}

					if (!DC::WinSock::Listen(m_listenSocket.get_socket(), SOMAXCONN)) {
						m_listenSocket.close_socket();
						m_listenSocket.set_socket(INVALID_SOCKET);
						return false;
					}

					for (DC::size_t i = 0; i < _Post_Size; i++) {
						auto AcceptIOptr = m_listenSocket.make_IOContext(64);
						if (!PostAccept(this->m_listenSocket, AcceptIOptr, this->m_AcceptEx)) {
							m_listenSocket.clear_IOContext();
							return false;
						}
					}

					return true;
				}

				inline void stop_listen() {
					m_listenSocket.close_socket();
					m_listenSocket.set_socket(INVALID_SOCKET);
				}

				void do_accept(IOCPSpace::IOContext* IOptr, LPFN_GETACCEPTEXSOCKADDRS GetAcceptExSockAddrsFunc) {
					DC::WinSock::Address *ClientAddress(nullptr), *ServerAddress(nullptr);
					int ClientLen(sizeof(DC::WinSock::Address)), ServerLen(sizeof(DC::WinSock::Address));

					GetAcceptExSockAddrsFunc(IOptr->m_wsabuf.buf, 0, sizeof(DC::WinSock::Address) + 16, sizeof(DC::WinSock::Address) + 16, reinterpret_cast<LPSOCKADDR*>(&ServerAddress), &ServerLen, reinterpret_cast<LPSOCKADDR*>(&ClientAddress), &ClientLen);
					std::string ClientAddressString(inet_ntoa(ClientAddress->sin_addr));

					std::unique_ptr<IOCPSpace::SocketContext, IOCPSpace::PSCdeleter> NewClientContext(IOCPSpace::IOCPAllocator<IOCPSpace::SocketContext>::make_norecord());
					if (isNull(NewClientContext.get())) {
						DC::WinSock::Close(IOptr->m_socket);
						return;
					}
					NewClientContext->set_socket(IOptr->m_socket);
					NewClientContext->set_client_address(*ClientAddress);

					if (!IOCPSpace::AssociateWithIOCP(this->m_iocp, NewClientContext.get()))
						return;

					if (!IOCPSpace::PostRecv(NewClientContext->make_IOContext(m_recvbuffer_length)))
						return;

					m_pscPool.put(*NewClientContext.release());

					PostAccept(this->m_listenSocket, IOptr, this->m_AcceptEx);

					invoke_usercode(m_onAcceptCallback, ClientAddressString);
				}

				void do_recv(IOCPSpace::SocketContext *Socketptr, IOCPSpace::IOContext *IOptr, const DC::size_t& length) {
					if (isNull(IOptr)) return;
					if (!IOptr->check_socket()) return;

					invoke_usercode(m_onRecvCallback, std::string(IOptr->m_wsabuf.buf, length), DC::WinSock::GetAddrString(Socketptr->get_client_address()), [Socketptr](const std::string& sendstr)->bool {
						return IOCPSpace::PostSend(Socketptr, sendstr);
					});

					PostRecv(IOptr);
				}

				void do_send(IOCPSpace::SocketContext *Socketptr, IOCPSpace::IOContext *IOptr, const DC::size_t& length) {
					if (isNull(IOptr)) return;
					if (!IOptr->check_socket()) return;

					if (length > IOptr->m_wsabuf.len)
						invoke_usercode(m_onExceptCallback, DC::Exception("do_send", "wsabuf.len<dwBytesTransfered"));
					else
						invoke_usercode(m_onSendCallback, std::string(IOptr->m_wsabuf.buf, length));

					Socketptr->remove_IOContext(IOptr);
				}

				inline void start_tp()noexcept {
					m_io_tp = new(std::nothrow) DC::ThreadPool(m_io_tp_threadnumber);
					m_usercode_tp = new(std::nothrow) DC::ThreadPool(m_usercode_tp_threadnumber);
				}

				inline bool check_tp()const noexcept {
					return !isNull(m_io_tp) && !isNull(m_usercode_tp);
				}

				inline void stop_tp()noexcept {
					if (!isNull(m_io_tp)) {
						delete m_io_tp;
						m_io_tp = nullptr;
					}
					if (!isNull(m_usercode_tp)) {
						delete m_usercode_tp;
						m_usercode_tp = nullptr;
					}
				}

				template <typename USERCODE, typename ...ARGS>
				inline void invoke_usercode(USERCODE&& usercode, ARGS&& ...args)const {
					m_usercode_tp->async([this, usercode, args...]() {//捕获参数列表是拷贝语义，防止参数失效
						try {
							usercode(args...);
						}
						catch (const DC::Exception& ex) {
							this->m_onExceptCallback(ex);
						}
						catch (...) {
							this->m_onExceptCallback(DC::Exception("run_usercode", "uncaught exception"));
						}
					});
				}

			private:
				LPFN_ACCEPTEX m_AcceptEx;//AcceptEx
				LPFN_GETACCEPTEXSOCKADDRS m_GetAcceptExSockAddrs;//GetAcceptExSockaddrs

				HANDLE m_iocp;
				IOCPSpace::SocketContext m_listenSocket;
				DC::ThreadPool *m_io_tp, *m_usercode_tp;

				//这几个用于通知cleaner返回
				std::condition_variable cleanerCV;
				std::mutex cleanerMut;
				std::atomic<bool> stopFlag;

				DC::WinSock::Address m_listenAddress;

				IOCPSpace::IOCPAllocator<IOCPSpace::SocketContext> m_pscPool;

				const DC::size_t m_io_tp_threadnumber, m_usercode_tp_threadnumber;
				DC::size_t m_recvbuffer_length, m_client_alive_time, m_cleaner_max_block_time;//default recvbuffer length==1024, default clientalivetime==15000, default cleanerwakeuptime==4000,
				std::chrono::milliseconds m_cleaner_sleep_time;
				//default cleaner_max_block_time==2000

				OnAcceptCallbackType m_onAcceptCallback;//ANY(const std::string& clientip)
				OnRecvCallbackType m_onRecvCallback;//ANY(const std::string& recvstr, const std::string& clientip, const DC::Web::IOCP::reply_type& reply)
				OnSendCallbackType m_onSendCallback;//ANY(const std::string& sendstr)
				OnExceptCallbackType m_onExceptCallback;//ANY(const DC::Exception& ex)
			};

		}

	}

}

#endif