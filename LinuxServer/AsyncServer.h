#pragma once

#include <map>
#include "fcmm.h"
#include <memory>
#include <thread>
#include <netdb.h>
#include "STXUtility.h"
#include "ThreadPool.h"
#include <condition_variable>

#include "concurrentqueue.h"

/////////////////////////////////////////////////////////////////////////////////////

class CAsyncServerTcpServerInfo;

/////////////////////////////////////////////////////////////////////////////////////
// CAsyncSubServerContext

class CAsyncSubServerContext
{
};

/////////////////////////////////////////////////////////////////////////////////////
// CAsyncServerPackage

struct CAsyncServerPackage
{
	char *_buf = nullptr;
	int _len = 0;
	void *_userData = nullptr;
	
	~CAsyncServerPackage()
	{
		free(_buf);
	}
};

/////////////////////////////////////////////////////////////////////////////////////
// CAsyncServerTcpClientContext

class CAsyncServerTcpClientContext
{
	friend class CAsyncServer;
	
public:
	CAsyncServerTcpClientContext();
	virtual ~CAsyncServerTcpClientContext();
	
protected:
	char *m_pRecvBuffer = nullptr;
	unsigned int m_cbBufferSize = 0;
	unsigned int m_cbWriteOffset = 0;
	
	moodycamel::ConcurrentQueue<CAsyncServerPackage*> _writeQueue;
	//tbb::concurrent_queue<CAsyncServerPackage*> _writeQueue;
	
	
protected:
	void AppendRecvData(char* lpData, unsigned int cbDataLen);
	void AppendSendData(char *buf, unsigned int dataLen, void* userData);
	void SkipRecvBuffer(unsigned int cbSkip);
	void* GetMessageBasePtr();
	unsigned int GetBufferedMessageLength();

protected:
	bool _deprecated = false;
	char _address[NI_MAXHOST + NI_MAXSERV + 4];
	int _clientSocket = 0;
	CAsyncServerTcpServerInfo *_tcpServer = nullptr;
	
};

/////////////////////////////////////////////////////////////////////////////////////
// CAsyncServerTcpServerInfo

class CAsyncServerTcpServerInfo
{
	friend class CAsyncServer ;
public:
	CAsyncServerTcpServerInfo()
	{
	}
	CAsyncServerTcpServerInfo(const CAsyncServerTcpServerInfo& val)
	{
		*this = val;
	}
	CAsyncServerTcpServerInfo&operator=(const CAsyncServerTcpServerInfo &val)
	{
		_port = val._port;
		_socket = val._socket;
		_epfd  = val._epfd;
		_listenThread = val._listenThread;
		_subServer = val._subServer;
		
		return *this;
	}
	
	unsigned int _port = 0 ;
	int _socket = 0 ;			//socked (fd) listening
	int _epfd = 0 ;				//epoll fd
	
	std::thread *_listenThread = nullptr ;
	std::shared_ptr<CAsyncSubServerContext> _subServer;
};

/////////////////////////////////////////////////////////////////////////////////////
// CAsyncServer

class CAsyncServer
{
public:
	CAsyncServer();
	virtual ~CAsyncServer();
	
protected:
	std::mutex m_mutexShutdown;
	std::condition_variable _shutdownVariable;
	
	fcmm::Fcmm<unsigned int, std::shared_ptr<CAsyncServerTcpServerInfo>> _tcpSubServers;
	CSTXHashMap<std::string, std::shared_ptr<CAsyncServerTcpClientContext>, 8, 1, CSTXDefaultStringHash<8, 1>> _tcpClients;
	CSTXHashMap<std::string, std::shared_ptr<CAsyncServerTcpClientContext>, 8, 1, CSTXDefaultStringHash<8, 1>> _tcpClientsDeprecated;
	
	ThreadPool _threadPool;

protected:
	bool CreateListenThread(int listenSocket, CAsyncServerTcpServerInfo *serverInfo);
	
protected:
	bool PreClientReceive(CAsyncServerTcpClientContext *clientContext, char *buf, int dataSize);
	void PreClientDisconnect(CAsyncServerTcpClientContext *clientContext);

	
protected:
	virtual std::shared_ptr<CAsyncServerTcpClientContext> OnCreateTcpClientContext(std::shared_ptr<CAsyncSubServerContext> subServer);
	virtual void OnTcpClientDataAvailable(CAsyncServerTcpClientContext *clientContext);
	virtual void OnTcpClientSendReady(CAsyncServerTcpClientContext *clientContext);
	virtual unsigned int IsClientDataReadable(CAsyncServerTcpClientContext *clientContext);
	virtual bool OnClientReceived(CAsyncServerTcpClientContext *clientContext, char *buf, int dataSize);
	virtual void OnClientDisconnect(CAsyncServerTcpClientContext *clientContext);
	virtual char* OnGetClientUniqueName(CAsyncServerTcpClientContext *clientContext);

public:
	bool CreateTcpSubServer(unsigned int port, void *userData = nullptr);
	int SendClientData(CAsyncServerTcpClientContext *clientContext, char *buf, unsigned int dataLen, void* userData = 0);
	void Wait();
};

