#include "RedisProxy.h"
#include "hiredis.h"
#include <vector>
#include <list>
#include <thread>
#include <chrono>
#include <assert.h>

#include <tbb/concurrent_queue.h>

#include <ws2tcpip.h>

namespace mc
{
	namespace util
	{
		void getHostByName(const char* hostName, std::list<std::string>& lstIP, const std::function<bool(std::string ip)>& cond)
		{
			lstIP.clear();

			struct addrinfo hints;
			struct addrinfo *result, *rp;
			struct sockaddr_in *sin;

			memset(&hints, 0x00, sizeof(hints));
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;

			if (getaddrinfo(hostName, nullptr, &hints, &result) != 0)
				return;

			int i = 0;
			for (rp = result; rp != NULL; rp = rp->ai_next, ++i)
			{
				if (rp->ai_family == AF_INET)
				{
					sin = (sockaddr_in *)rp->ai_addr;
					std::string str = inet_ntoa(sin->sin_addr);
					if (cond(str) == false)
						continue;
					lstIP.push_back(str);
				}
			}

			freeaddrinfo(result);
		}

		void getHostByName(const char* hostName, std::list<std::string>& lstIP)
		{
			getHostByName(hostName, lstIP, [](std::string){ return true; });
		}
	}

	class CRedisProxy::CImpl
	{
	public : 
		enum { MAX_ARG_NUM = 5, };
	protected:
		struct stRequestContext_t
		{
			std::vector<std::string>		lstCommand_;
			callback_t		callback_;

			stRequestContext_t() : lstCommand_(), callback_() {}
			stRequestContext_t(const CRedisProxy::callback_t& callback,std::string command ) : callback_(callback) { lstCommand_.push_back(std::move(command)); }

			template < typename T>
			void arg_back_inserter(std::vector<std::string>& lstCommand, T& arg)
			{
				lstCommand.push_back(arg);
			}

			template < typename T ,typename... Args>
			void arg_back_inserter(std::vector<std::string>& lstCommand, T& arg, Args... args)
			{
				lstCommand.push_back(arg);
				arg_back_inserter(lstCommand, args...);
			}

			template <typename... Args>
			stRequestContext_t(const CRedisProxy::callback_t& callback,std::string command, Args... args) : callback_(callback)
			{
				lstCommand_.push_back(std::move(command));
				arg_back_inserter(lstCommand_, args...);
			}
			stRequestContext_t(const stRequestContext_t& rhs) : lstCommand_(rhs.lstCommand_), callback_(rhs.callback_) {}
		};
		typedef tbb::concurrent_queue<stRequestContext_t>		queRequest_t;

		struct stResponse_t
		{
			bool						success_;
			std::vector<std::string>	lstString_;
			callback_t					callback_;

			stResponse_t() : success_(true), lstString_(), callback_(){}
			stResponse_t(bool success, std::vector<std::string> lstString, const callback_t& callback) : success_(success), lstString_(std::move(lstString)), callback_(callback){}
		};
		typedef tbb::concurrent_queue<stResponse_t>				queResponse_t;

	protected:
		redisContext*			context_;

		std::string				target_;
		unsigned short			port_;

		bool					isRunning_;
		std::thread				thread_;
		queRequest_t			queRequest_;
		queResponse_t			queResponse_;

	public:
		CImpl() : context_(nullptr), isRunning_(false), thread_(), queRequest_() {}
		~CImpl(){ if (context_ != nullptr) redisFree(context_); }

	public:
		bool		init(const char* target, unsigned short port);
		void		run();
		void		stop();

		void		sendCommand(const CRedisProxy::callback_t& callback, const char* cmd);
		template <typename... Args>
		void		sendCommand(const CRedisProxy::callback_t& callback, const char* cmd, Args... args) { queRequest_.push(stRequestContext_t(callback, cmd, args...)); }

		size_t		handleResultCallback();

	protected:
		bool		_connect(const char* target, unsigned short port);
		void		_run();
	};

	bool CRedisProxy::CImpl::init(const char* target, unsigned short port)
	{
		assert(isRunning_ == false);

		target_ = target;
		port_ = port;

		if (_connect(target_.c_str(), port_) == false)
			return false;

		return true;
	}

	void CRedisProxy::CImpl::run()
	{
		if (isRunning_ == true)
			return;
		isRunning_ = true;
		thread_ = std::thread([=]{ _run(); });
	}

	void CRedisProxy::CImpl::stop()
	{
		if (isRunning_ == false)
			return;

		isRunning_ = false;
		thread_.join();
	}

	void CRedisProxy::CImpl::sendCommand(const CRedisProxy::callback_t& callback, const char* cmd)
	{
		queRequest_.push(stRequestContext_t(callback,cmd));
	}

	size_t CRedisProxy::CImpl::handleResultCallback()
	{
		size_t processNum = 0;

		stResponse_t response;
		while (queResponse_.try_pop(response) == true)
		{
			response.callback_(response.success_, response.lstString_);
			++processNum;
		}

		return processNum;
	}

	bool CRedisProxy::CImpl::_connect(const char* target, unsigned short port)
	{
		std::list<std::string> lstIP;
		util::getHostByName(target, lstIP);

		for(std::string ip : lstIP)
		{
			context_ = redisConnect(ip.c_str(), port);
			if (context_ != nullptr)
				break;
		}

		return context_ != nullptr;
	}

	void CRedisProxy::CImpl::_run()
	{
		while (isRunning_ == true)
		{
			if (queRequest_.empty() == true)
			{	//잠시 대기후 진행
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			stRequestContext_t requestContext;
			if (queRequest_.try_pop(requestContext) == false)
				continue;

			redisReply* reply = nullptr;
			if (requestContext.lstCommand_.size() == 1)
			{
				reply = (redisReply*)redisCommand(context_, requestContext.lstCommand_.at(0).c_str());
				if (reply == nullptr)
				{
					printf("RedisProxyError(%d,%s)", context_->err, context_->errstr);

					redisFree(context_);
					if (_connect(target_.c_str(), port_) == false)
						continue;
					reply = (redisReply*)redisCommand(context_, requestContext.lstCommand_.at(0).c_str());
					if (reply == nullptr)
						continue;
				}
			}
			else
			{
				size_t argvLen[MAX_ARG_NUM] = { 0, };
				const char* argv[MAX_ARG_NUM] = { nullptr, };
				for (unsigned int x = 0; x<requestContext.lstCommand_.size(); ++x)
				{
					argv[x] = requestContext.lstCommand_.at(x).c_str();
					argvLen[x] = requestContext.lstCommand_.at(x).size();
				}
				reply = (redisReply*)redisCommandArgv(context_, (int)requestContext.lstCommand_.size(), &argv[0], &argvLen[0]);
				if (reply == nullptr)
				{
					printf("RedisProxyError(%d,%s)", context_->err, context_->errstr);

					redisFree(context_);
					if (_connect(target_.c_str(), port_) == false)
						continue;
					reply = (redisReply*)redisCommandArgv(context_, (int)requestContext.lstCommand_.size(), &argv[0], &argvLen[0]);
					if (reply == nullptr)
						continue;
				}
			}

			if (requestContext.callback_ != nullptr)
			{
				bool success = true;
				std::vector<std::string> lstString;
				if (reply->type == REDIS_REPLY_STRING)
					lstString.push_back(reply->str);
				else if (reply->type == REDIS_REPLY_INTEGER)
					lstString.push_back(std::to_string(reply->integer));
				else if (reply->type == REDIS_REPLY_ARRAY)
				{
					for (size_t x = 0; x<reply->elements; ++x)
						lstString.push_back(reply->element[x]->str);
				}
				else if (reply->type == REDIS_REPLY_STATUS)
					lstString.push_back(reply->str);
				else if (reply->type == REDIS_REPLY_ERROR)
				{
					lstString.push_back(reply->str);
					success = false;
				}

				queResponse_.push(stResponse_t(success, std::move(lstString), requestContext.callback_));
			}

			freeReplyObject(reply);
		}
	}

	/////////////////////////////////////////////////////////////////////CRedisProxy
	CRedisProxy::CRedisProxy()
	{
	}

	CRedisProxy::~CRedisProxy()
	{
		for(auto iter : lstInstance_)
		{
			iter->stop();
			delete iter;
		}
		lstInstance_.clear();
	}

	bool CRedisProxy::init(unsigned int instanceId, const char* target, unsigned short port)
	{
		if (instanceId >= lstInstance_.size())
		{
			for (size_t x = lstInstance_.size(); x <= instanceId;++x)
				lstInstance_.push_back(nullptr);
		}

		if (lstInstance_[instanceId] != nullptr)
			return false;

		CImpl* instance = new CImpl;
		if (instance->init(target, port) == false)
		{
			delete instance;
			return false;
		}

		lstInstance_[instanceId] = instance;
		return true;
	}

	void CRedisProxy::run(unsigned int instanceId)
	{
		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;

		return instance->run();
	}

	void CRedisProxy::run()
	{
		for(auto iter : lstInstance_)
		{
			iter->run();
		}
	}

	void CRedisProxy::stop(unsigned int instanceId)
	{
		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;
		return instance->stop();
	}

	void CRedisProxy::stop()
	{
		for(auto iter : lstInstance_)
		{
			iter->stop();
		}
	}

	void CRedisProxy::sendCommand(unsigned int instanceId, const callback_t& callback, const char* fmt, ...)
	{
		va_list	ap;
		char tmpBuffer[2048];

		va_start(ap, fmt);
		vsnprintf(tmpBuffer, sizeof(tmpBuffer), fmt, ap);
		va_end(ap);

		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;
		instance->sendCommand(callback, tmpBuffer);
	}

	void CRedisProxy::sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1)
	{
		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;
		instance->sendCommand(callback, cmd, arg1);
	}

	void CRedisProxy::sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1, const char* arg2)
	{
		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;
		instance->sendCommand(callback, cmd, arg1, arg2);
	}

	void CRedisProxy::sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1, const char* arg2, const char* arg3)
	{
		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;
		instance->sendCommand(callback, cmd, arg1, arg2, arg3);
	}

	void CRedisProxy::sendCommandArgv(unsigned int instanceId, const callback_t& callback, const char* cmd, const char* arg1, const char* arg2, const char* arg3, const char* arg4)
	{
		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;
		instance->sendCommand(callback, cmd, arg1, arg2, arg3, arg4);
	}

	void CRedisProxy::sendCommand2(unsigned int instanceId, const char* fmt, ...)
	{
		va_list	ap;
		char tmpBuffer[2048];

		va_start(ap, fmt);
		vsnprintf(tmpBuffer, sizeof(tmpBuffer), fmt, ap);
		va_end(ap);

		auto instance = _getInstance(instanceId);
		if (instance == nullptr)
			return;
		instance->sendCommand(nullptr, tmpBuffer);
	}

	size_t CRedisProxy::handleResultCallback()
	{
		size_t handleResultNum = 0;
		for(auto iter : lstInstance_)
		{
			handleResultNum += iter->handleResultCallback();
		}
		return handleResultNum;
	}
}