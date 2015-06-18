// RedisProxy.cpp : 콘솔 응용 프로그램에 대한 진입점을 정의합니다.
//

#ifdef  WIN32
#include <WinSock2.h>
#endif  //WIN32

#include "RedisProxy.h"

#include <thread>

int _tmain(int argc, _TCHAR* argv[])
{
#ifdef  WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif  //WIN32

	mc::CRedisProxy redisProxy;
	redisProxy.init(0, "localhost", 6379);
	redisProxy.run();

	redisProxy.sendCommand2Argv(0, "HGET", "1", "2", "3");

	while (1)
	{
		redisProxy.handleResultCallback();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return 0;
}

