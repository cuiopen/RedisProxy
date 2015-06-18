#pragma once

#include <functional>
#include <vector>

namespace mc
{
	class CRedisProxy
	{
	public:
		typedef std::function<void(bool, const std::vector<std::string>&)>	callback_t;

	protected:
		class	CImpl;
		typedef std::vector<CImpl*>		lstInstance_t;

		lstInstance_t	lstInstance_;

	public:
		CRedisProxy();
		~CRedisProxy();

	public:
		bool	init(unsigned int instanceId, const char* target, unsigned short port);
		void	run(unsigned int instanceId);
		void	run();
		void	stop(unsigned int instanceId);
		void	stop();

		void	sendCommand(unsigned int instanceId, const callback_t& callback, const char* fmt, ...);
		void	sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1);
		void	sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1, const char* arg2);
		void	sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1, const char* arg2, const char* arg3);
		void	sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1, const char* arg2, const char* arg3, const char* arg4);

		//callback이 없는 함수
		void	sendCommand2(unsigned int instanceId, const char* fmt, ...);
		void	sendCommand2Argv(unsigned int instanceId, const char* cmd, const char* arg1) { sendCommandArgv(instanceId, nullptr, cmd, arg1); }
		void	sendCommand2Argv(unsigned int instanceId, const char* cmd, const char* arg1, const char* arg2) { sendCommandArgv(instanceId, nullptr, cmd, arg1, arg2); }
		void	sendCommand2Argv(unsigned int instanceId, const char* cmd, const char* arg1, const char* arg2, const char* arg3) { sendCommandArgv(instanceId, nullptr, cmd, arg1, arg2, arg3); }
		void	sendCommand2Argv(unsigned int instanceId, const char* cmd, const char* arg1, const char* arg2, const char* arg3, const char* arg4) { sendCommandArgv(instanceId, nullptr, cmd, arg1, arg2, arg3, arg4); }

		size_t	handleResultCallback();

	protected : 
		CImpl*	_getInstance(unsigned int instanceId) const
		{
			if (instanceId >= lstInstance_.size())
				return nullptr;
			return lstInstance_[instanceId];
		}
	};
}