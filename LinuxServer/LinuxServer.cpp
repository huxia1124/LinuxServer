#include <iostream>
#include "AsyncServer.h"
#include <unistd.h>

using namespace std;

int main(int argc, char *argv[])
{
	unsigned int port = 9988;
	
	CAsyncServer server;
	if (!server.CreateTcpSubServer(9988, nullptr))
	{
		cout << "Error: TCP Server Failed to start! Port=" << port << endl;
		return 1;
	}
	cout << "TCP Server started, listening on port " << port << endl;
	server.Wait();
	
	return 0;
}