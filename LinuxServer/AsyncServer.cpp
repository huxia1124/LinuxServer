#include "AsyncServer.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <utility>

#include <iostream>

/////////////////////////////////////////////////////////////////////////////////////
// CAsyncServerTcpClientContext

CAsyncServerTcpClientContext::CAsyncServerTcpClientContext()
{
	m_cbBufferSize = 16384;
	m_pRecvBuffer = (char*)malloc(m_cbBufferSize);
	m_cbWriteOffset = 0;
}

CAsyncServerTcpClientContext::~CAsyncServerTcpClientContext()
{
	free(m_pRecvBuffer);
}

unsigned int CAsyncServerTcpClientContext::GetBufferedMessageLength()
{
	return m_cbWriteOffset;
}

void CAsyncServerTcpClientContext::AppendRecvData(char* lpData, unsigned int cbDataLen)
{
	//UpdateLastDataReceiveTime();

	//由于每次处理收到的消息包完成之后才会再次调用IssueClientRead。此处不存在多线程同时进入此函数的情况
	unsigned int cbOldBufferSize = m_cbBufferSize;
	while ((unsigned int)cbDataLen + m_cbWriteOffset > m_cbBufferSize)
		m_cbBufferSize *= 2;

	if (m_cbBufferSize > cbOldBufferSize)
	{
		m_pRecvBuffer = (char*)realloc(m_pRecvBuffer, m_cbBufferSize);
	}

	memcpy(m_pRecvBuffer + m_cbWriteOffset, lpData, cbDataLen);
	m_cbWriteOffset += cbDataLen;
}

void CAsyncServerTcpClientContext::AppendSendData(char *buf, unsigned int dataLen, void* userData)
{
	CAsyncServerPackage *package = new CAsyncServerPackage();
	
	char *bufCopy = (char*)malloc(dataLen);
	memcpy(bufCopy, buf, dataLen);
	
	package->_buf = bufCopy;
	package->_len = dataLen;
	package->_userData = userData;
	
	
	//_writeQueue.push(package);
	_writeQueue.enqueue(package);
}

void CAsyncServerTcpClientContext::SkipRecvBuffer(unsigned int cbSkip)
{
	//由于每次处理收到的消息包完成之后才会再次调用IssueClientRead。此处不存在多线程同时进入此函数的情况
	memmove(m_pRecvBuffer, m_pRecvBuffer + cbSkip, m_cbWriteOffset - cbSkip);
	m_cbWriteOffset -= cbSkip;
}

void* CAsyncServerTcpClientContext::GetMessageBasePtr()
{
	return m_pRecvBuffer;
}

/////////////////////////////////////////////////////////////////////////////////////
// CAsyncServer

CAsyncServer::CAsyncServer()
		: _threadPool(10)
{
}


CAsyncServer::~CAsyncServer()
{
}

bool CAsyncServer::CreateListenThread(int listenSocket, CAsyncServerTcpServerInfo *serverInfo)
{
	int epfd = serverInfo->_epfd;
	auto subServer = serverInfo->_subServer;
	
	serverInfo->_listenThread = new std::thread([=](void *data) {
			epoll_event ev;
			epoll_event events[20];
			for (;;)
			{
				int n = epoll_wait(epfd, events, 20, 500);
				for (int i = 0; i < n; ++i)
				{
					if (events[i].data.fd == listenSocket) //New connection coming
					{
						struct sockaddr_in clientaddr;
						socklen_t in_len = sizeof(clientaddr); 
						int connfd = accept(listenSocket, (sockaddr *)&clientaddr, &in_len); //accept this connection
						
						auto clientContext = OnCreateTcpClientContext(subServer);
						
						char hbuf[NI_MAXHOST];
						char sbuf[NI_MAXSERV];
						getnameinfo((sockaddr*)&clientaddr, in_len, hbuf, sizeof hbuf, sbuf, sizeof sbuf, NI_NUMERICHOST | NI_NUMERICSERV); //flag参数:以数字名返回
						strcpy(clientContext->_address, hbuf);
						strcat(clientContext->_address, ":");
						strcat(clientContext->_address, sbuf);
						
						auto key = OnGetClientUniqueName(clientContext.get());
						_tcpClients.lock(key);
						_tcpClients[key] = clientContext;
						_tcpClients.unlock(key);						

						clientContext->_clientSocket = connfd;
						clientContext->_tcpServer = serverInfo;
						
						fcntl(connfd, F_SETFL, O_NONBLOCK);
						
						ev.data.ptr = clientContext.get();
						ev.events = EPOLLIN | EPOLLET;
						epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev); //Add the fd to epoll listening queue
					}
					else if (events[i].events & EPOLLIN) //Data received. read from socket
					{
						CAsyncServerTcpClientContext *clientContext = (CAsyncServerTcpClientContext*)events[i].data.ptr;
						
						_threadPool.enqueue([=]() {
							//std::thread::id this_id = std::this_thread::get_id();
							//std::cout << "_threadPool.enqueue - " << this_id << std::endl;
							
							OnTcpClientDataAvailable(clientContext);
						});
						
					}
					else if (events[i].events & EPOLLOUT) //Data waiting to be sent. write to socket
					{
						CAsyncServerTcpClientContext *clientContext = (CAsyncServerTcpClientContext*)events[i].data.ptr;
						
						_threadPool.enqueue([=]() {							
							OnTcpClientSendReady(clientContext);
						});
						
						ev.data.ptr = clientContext;
						ev.events = EPOLLIN | EPOLLET;
						epoll_ctl(epfd, EPOLL_CTL_MOD, clientContext->_clientSocket, &ev); //Add the fd to epoll listening queue
					}
					else
					{
						//Other cases
					}
				}
			}
		},
		nullptr);
	                                            
	
	return true;
}

bool CAsyncServer::PreClientReceive(CAsyncServerTcpClientContext *clientContext, char *buf, int dataSize)
{
	bool bResult = true;
	clientContext->AppendRecvData(buf, dataSize);
	unsigned int dwMessageSize = 0;
	
	while ((dwMessageSize = IsClientDataReadable(clientContext)) > 0)
	{
		bResult = OnClientReceived(clientContext, (char*)clientContext->GetMessageBasePtr(), dwMessageSize);
		clientContext->SkipRecvBuffer(dwMessageSize);
	}

	return bResult;
}

void CAsyncServer::PreClientDisconnect(CAsyncServerTcpClientContext *clientContext)
{
	OnClientDisconnect(clientContext);
	char *uniqueName = OnGetClientUniqueName(clientContext);
	auto it = _tcpClients.find(uniqueName);
	if (it != _tcpClients.end(uniqueName))
	{
		it->second->_deprecated = true;
		_tcpClientsDeprecated.lock(uniqueName);
		_tcpClientsDeprecated[uniqueName] = it->second;
		_tcpClientsDeprecated.unlock(uniqueName);
	}
	close(clientContext->_clientSocket);	//close fd
}

bool CAsyncServer::CreateTcpSubServer(unsigned int port, void *userData)
{
	auto it = _tcpSubServers.find(port);
	if (it != _tcpSubServers.end())
	{
		return false;
	}

	int listenSocket = 0;
	if ((listenSocket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		//throw exception("Can't create socket");
		printf("Can't create socket.\n");
		return false;
	}
  
	//int yes;
	//if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
	//{
		//throw exception("Can't set socket options");
	//}
  
  
	sockaddr_in serveraddr;
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = INADDR_ANY;
	serveraddr.sin_port = htons(port);
  
	memset(&(serveraddr.sin_zero), '\0', 8);

	if (bind(listenSocket, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) == -1)
	{
		//throw exception("Can't bind to the socket");
		printf("Can't bind to the socket.\n");
		close(listenSocket);
		return false;
	}
	if (listen(listenSocket, SOMAXCONN) == -1)
	{
		//throw exception("Can't listen on the socket");
		printf("Can't listen on the socket.\n");
		close(listenSocket);
		return false;
	}
	
	int epfd = epoll_create(100);
	epoll_event ev;	
	ev.data.fd = listenSocket;
	ev.events = EPOLLIN;
	epoll_ctl(epfd, EPOLL_CTL_ADD, listenSocket, &ev);
	
	std::shared_ptr<CAsyncServerTcpServerInfo> si(new CAsyncServerTcpServerInfo());
	si->_epfd = epfd;
	
	if (!CreateListenThread(listenSocket, si.get()))
	{
		printf("CreateListenThread failed.\n");
		close(epfd);
		return false;
	}
	
	fcmm::Fcmm<unsigned int, std::shared_ptr<CAsyncServerTcpServerInfo>>::Entry entry;
	entry.first = port;
	entry.second = si;
	_tcpSubServers.insert(entry);
	
	
	return true;
}

std::shared_ptr<CAsyncServerTcpClientContext> CAsyncServer::OnCreateTcpClientContext(std::shared_ptr<CAsyncSubServerContext> subServer)
{
	return std::make_shared<CAsyncServerTcpClientContext>();
}

void CAsyncServer::OnTcpClientDataAvailable(CAsyncServerTcpClientContext *clientContext)
{
	const int bufferSize = 4096;
	char buf[bufferSize];
	int n = read(clientContext->_clientSocket, buf, bufferSize);
	while (n >= 0)
	{
		if (errno == EINTR)
		{
			return;
		}
		if (n < 0)
		{
			PreClientDisconnect(clientContext);
			return;
		}
		if (n == 0)
		{
			PreClientDisconnect(clientContext);
			return;
		}
		
		PreClientReceive(clientContext, buf, n);
		n = read(clientContext->_clientSocket, buf, bufferSize);
	}
	
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
	{
	}
	else
	{
		PreClientDisconnect(clientContext);
	}
}

void CAsyncServer::OnTcpClientSendReady(CAsyncServerTcpClientContext *clientContext)
{
	CAsyncServerPackage *package = nullptr;
	//while (clientContext->_writeQueue.unsafe_size() > 0)
	while (clientContext->_writeQueue.size_approx() > 0)
	{
		//if (clientContext->_writeQueue.try_pop(package))
		if (clientContext->_writeQueue.try_dequeue(package))
		{
			write(clientContext->_clientSocket, package->_buf, package->_len);
		}
	}
}

unsigned int CAsyncServer::IsClientDataReadable(CAsyncServerTcpClientContext *clientContext)
{
	return clientContext->GetBufferedMessageLength();
}

bool CAsyncServer::OnClientReceived(CAsyncServerTcpClientContext *clientContext, char *buf, int dataSize)
{
	printf("%d bytes received!\n", dataSize);
	
	SendClientData(clientContext, buf, dataSize);
	
	return true;
}

void CAsyncServer::OnClientDisconnect(CAsyncServerTcpClientContext *clientContext)
{
	printf("OnClientDisconnect!\n");
}

char* CAsyncServer::OnGetClientUniqueName(CAsyncServerTcpClientContext *clientContext)
{
	return clientContext->_address;
}

int CAsyncServer::SendClientData(CAsyncServerTcpClientContext *clientContext, char *buf, unsigned int dataLen, void* userData)
{
	clientContext->AppendSendData(buf, dataLen, userData);
	
	epoll_event ev;
	ev.data.ptr = clientContext;
	ev.events = EPOLLOUT | EPOLLET;
	
	int epfd = clientContext->_tcpServer->_epfd;
	epoll_ctl(epfd, EPOLL_CTL_MOD, clientContext->_clientSocket, &ev);		//Modify the flags
	
	return 0;
}

void CAsyncServer::Wait()
{
	std::unique_lock<std::mutex> mlock(m_mutexShutdown);
	_shutdownVariable.wait(mlock);
}