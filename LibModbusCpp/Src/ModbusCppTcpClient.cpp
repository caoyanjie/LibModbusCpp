#include "ModbusCppTcpClient.h"
#include "modbus.h"
#include <WinSock2.h>
#include <Windows.h>
#include <thread>

ModbusCppTcpClient::ModbusCppTcpClient()
	: m_connected(false)
	, m_readBuffer{ 0 }
	, m_modbusClient(NULL)
	, m_timeoutSec(2)
	, m_timeoutUsec(0)
	, m_retries(5)
	, m_requestFailedCallback(nullptr)
	, m_receivedDataCallback(nullptr)
{
	std::jthread _checkConnectionStateThread(&ModbusCppTcpClient::checkConnectionStateThread, this);
	_checkConnectionStateThread.detach();

	std::jthread _processMsgThread(&ModbusCppTcpClient::processMsgThread, this);
	_processMsgThread.detach();
}

ModbusCppTcpClient::~ModbusCppTcpClient()
{
	if (NULL != m_modbusClient)
	{
		if (m_connected)
		{
			// 关闭连接
			disconnectServer();
		}

		// 释放内存
		modbus_free(m_modbusClient);
	}
}

bool ModbusCppTcpClient::setTimeout(uint64_t msec)
{
	uint64_t _usec = msec * 1000;
	m_timeoutSec = _usec / 1000000;
	m_timeoutUsec = _usec % 1000000;

	if (NULL == m_modbusClient)
	{
		return true;
	}

	const int _ret = modbus_set_response_timeout(m_modbusClient, m_timeoutSec, m_timeoutUsec);
	return _ret == 0;
}

void ModbusCppTcpClient::setRetries(const uint8_t retries)
{
	m_retries = retries;
}

void ModbusCppTcpClient::setRequestFailedCallback(const std::function<void()> callback)
{
	m_requestFailedCallback = callback;
}

void ModbusCppTcpClient::setReceivedDataCallback(const std::function<void(const uint16_t, const std::vector<uint16_t>)> callback)
{
	m_receivedDataCallback = callback;
}

void ModbusCppTcpClient::setConnectionStateChangedCallback(const std::function<void(bool)> callback)
{
	m_connectionStateChangedCallback = callback;
}

// 连接服务器
bool ModbusCppTcpClient::connectServer(const std::string& serverHost, const uint16_t serverPort, const int slaveId)
{
	if (NULL != m_modbusClient)
	{
		if (m_connected)
		{
			disconnectServer();
		}
		modbus_free(m_modbusClient);
	}

	m_modbusClient = modbus_new_tcp(serverHost.c_str(), serverPort);
	if (NULL == m_modbusClient)
	{
		// error
		return false;
	}

	bool _ret = modbus_set_slave(m_modbusClient, slaveId);
	if (0 != _ret)
	{
		// error
		return false;
	}

	// 设置超时
	_ret = modbus_set_response_timeout(m_modbusClient, m_timeoutSec, m_timeoutUsec);
	if (0 != _ret)
	{
		// error
		return false;
	}

	// ?? 判断如果已经连接，是否要返回true，如果已经连接重复连，下面函数会不会返回false

	//modbus_set_retries(m_modbusClient, 3); // 设置重传次数

	int _connected = modbus_connect(m_modbusClient);
	//if (0 != _ret)
	if (-1 == _connected)
	{
		std::lock_guard<std::mutex> _lock(m_checkConnectionStateLock);
		m_connected = false;
		return false;
	}

	// test
	//int _socket = modbus_get_socket(m_modbusClient);
	//int _keepAlive = 1;
	//if (setsockopt(_socket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&_keepAlive, sizeof(_keepAlive)) == SOCKET_ERROR)
	//{
	//    std::cout << "set error" << std::endl;
	//}
	////tcp_keepalive keepalive_opts;
	// test

	// 唤醒状态检测线程工作
	m_checkConnectionStateLock.lock();
	m_connected = true;
	m_checkConnectionStateLock.unlock();
	m_checkConnectionStateCondition.notify_one();

	// 通知出去
	if (nullptr != m_connectionStateChangedCallback)
	{
		m_connectionStateChangedCallback(true);
	}
	return true;
}

// 断开连接
void ModbusCppTcpClient::disconnectServer()
{
	if (NULL == m_modbusClient)
	{
		return;
	}

	if (!m_connected)
	{
		return;
	}

	modbus_close(m_modbusClient);
}

// 检查连接状态
bool ModbusCppTcpClient::isConnected()
{
	std::lock_guard<std::mutex> _lock(m_lockTest);
	if (NULL == m_modbusClient)
	{
		return false;
	}

	const int _sock = modbus_get_socket(m_modbusClient);
	if (_sock == -1)
	{
		return false;
	}

	fd_set read_fds;
	FD_ZERO(&read_fds);         // 清空集合（所有位设为0）
	FD_SET(_sock, &read_fds);   // 将套接字_sock加入read_fds（对应位设为1），表示要监视其“可读性”。 调用 select() 前需用 FD_SET 将目标套接字加入集合，调用后 select 会修改该集合（仅保留已准备好可读的套接字）

	// _timeout 的 tv_sec 和 tv_usec 都是 0，表示非阻塞检查（立即返回）
	struct timeval _timeout;
	_timeout.tv_sec = 0;
	_timeout.tv_usec = 0;

	int rc = select((_sock + 1), &read_fds, NULL, NULL/*不监视异常（exceptfds）*/, &_timeout/*超时时间，指定select等待的最长时间*/);
	switch (rc)
	{
	case -1:
		// select调用失败，未完成任何监视
		std::cout << "select error -1" << std::endl;
		// return true;
		return false;
	case 0:
		// 超时（未发生任何事件），可继续循环等待
		// 正常连接状态，超时(在 _timeout 指定的时间内，没有任何被监视的套接字准备好可读)，对于 TCP 监听套接字：没有新的客户端连接请求； 对于已连接的 TCP 套接字：接收缓冲区中没有数据
		return true;
	default:
		// 有 n 个被监视的套接字在 read_fds 集合中准备好可读（即这些套接字满足“可读”条件）
		// 这时候需要用 FD_ISSET 来检查具体是哪个套接字
		// read_fds是输入输出参数，调用前需要设置要监视的套接字，调用后会被修改为只包含准备好的套接字
		if (FD_ISSET(_sock, &read_fds)) // FD_ISSET 是一个位检查宏，用于判断某个套接字是否在 select() 修改后的 fd_set 集合中
		{
			// 对于监听套接字（listen后的），可读意味着有新的连接请求；
			// 对于已连接的 TCP 套接字，可读意味着接收缓冲区有数据，或者对方关闭了连接，进一步检测
			// 有数据可读或连接状态有变化，进一步检查, 例如，可以尝试读取数据或发送一个小的请求来确认连接
			char _buf[1024];
			int _receivedSize = recv(_sock, _buf, sizeof(_buf), 0/*MSG_PEEK*/);
			std::cout << "clear error buf size:" << _receivedSize << std::endl;
			// switch (_receivedSize)
			// {
			// case -1:
			//     // 正在接收数据时，远端关闭了连接
			//     std::cout << "disconnected when reading data" << std::endl;
			//     return false;
			// case 0:
			//     // 仅适用于 TCP 套接字（UDP无连接概念），表示对方已关闭连接（发送了 FIN 包）
			//     // 没在接收数据时，远端关闭了连接
			//     std::cout << "disconnected when not reading data" << std::endl;
			//     return false;
			// default:
			//     // 盒盖一段时间会进入这里，读写都会失败，但是确实是把值写进服务器了
			//     //std::cout << "errno:" << errno << ", _receivedSize:" << _receivedSize << ", read value:" << (int)(_buf[0]) <<  std::endl;
			//     char _v[1024];
			//     int _errDataSize = recv(_sock, _buf, sizeof(_v), 0);
			//     std::cout << "read errordata len:" << _errDataSize << std::endl;
			//     return true;
			// }
			return _receivedSize > 0;
		}
	}

	return true;
}

// 同步写数据
bool ModbusCppTcpClient::writeRegistersSync(const uint16_t startAddress, const std::vector<uint16_t>& data)
{
	std::lock_guard<std::mutex> _lock(m_lockTest);
	// 检查是否已初始化成功
	if (!m_connected)
	{
		return false;
	}

	// 检查数据长度
	if (data.size() > DATA_LEN_MAX)
	{
		return false;
	}

	// 构造数据
	for (size_t i = 0; i < data.size(); ++i)
	{
		m_writeBuffer[i] = data.at(i);
	}

	// 写入数据
	for (int i = 0; i < m_retries; ++i)
	{
		const int _writeNum = modbus_write_registers(m_modbusClient, startAddress, static_cast<int>(data.size()), m_writeBuffer);
		if (_writeNum == data.size())
		{
			// 写入成功
			return true;
		}
		else
		{
			//std::cout << "写入失败" << std::endl;
		}
	}

	// 写入失败
	if (nullptr != m_requestFailedCallback)
	{
		m_requestFailedCallback();
	}
	return false;
}

// 同步读数据
std::optional<std::vector<uint16_t> > ModbusCppTcpClient::readRegistersSync(const uint16_t startAddress, const uint8_t dataLen)
{
	std::lock_guard<std::mutex> _lock(m_lockTest);
	// 检查是否已初始化成功
	if (!m_connected)
	{
		return std::nullopt;
	}

	// 检查数据长度
	if (dataLen > DATA_LEN_MAX)
	{
		return std::nullopt;
	}

	// 读取数据
	memset(m_readBuffer, 0, DATA_LEN_MAX);  // 清空缓存值
	for (int i = 0; i < m_retries; ++i)
	{
		const int _readNum = modbus_read_registers(m_modbusClient, startAddress, dataLen, m_readBuffer);

		// 检查读取结果
		if (_readNum == dataLen)
		{
			if (i > 0)
			{
				std::cout << "retry times: " << i + 1 << "read successed" << std::endl;
			}
			// 组装读到的数据，返回结果
			std::vector<uint16_t> _data;
			for (size_t j = 0; j < dataLen; ++j)
			{
				_data.push_back(m_readBuffer[j]);
			}
			return std::optional<std::vector<uint16_t>>(_data);
		}
	}

	// 服务器未响应请求
	if (nullptr != m_requestFailedCallback)
	{
		m_requestFailedCallback();
	}
	std::string _error = modbus_strerror(errno);
	if (_error != "No error")
	{
		std::cout << modbus_strerror(errno) << std::endl; // No error
	}

	return std::nullopt;
}

// 同步读写数据
std::optional<std::vector<uint16_t> > ModbusCppTcpClient::writeAndReadRegistersSync(const uint16_t writeStartAddress, const std::vector<uint16_t>& writeData, const uint16_t readStartAddress, const uint8_t readLen)
{
	std::lock_guard<std::mutex> _lock(m_lockTest);
	// 检查是否已初始化成功
	if (!m_connected)
	{
		return std::nullopt;
	}

	// 检查数据长度
	if (writeData.size() > DATA_LEN_MAX || readLen > DATA_LEN_MAX)
	{
		return std::nullopt;
	}

	// 构造写数据
	for (int i = 0; i < writeData.size(); ++i)
	{
		m_writeBuffer[i] = writeData.at(i);
	}

	// 清空读缓存
	memset(m_readBuffer, 0, DATA_LEN_MAX);

	// 读写数据
	for (int i = 0; i < m_retries; ++i)
	{
		int _readNum = modbus_write_and_read_registers(m_modbusClient, writeStartAddress, static_cast<int>(writeData.size()), m_writeBuffer, readStartAddress, readLen, m_readBuffer);

		// 检查读取结果
		if (_readNum == readLen)
		{
			// 组装读到的数据，返回结果
			std::vector<uint16_t> _data;
			for (int i = 0; i < readLen; ++i)
			{
				_data.push_back(m_readBuffer[i]);
			}
			return std::optional<std::vector<uint16_t>>(_data);
		}
	}

	if (nullptr != m_requestFailedCallback)
	{
		m_requestFailedCallback();
	}
	return std::nullopt;
}

// 异步写数据
bool ModbusCppTcpClient::writeRegistersAsync(const uint16_t startAddress, const std::vector<uint16_t>& data)
{
	// 检查是否已初始化成功
	if (!m_connected)
	{
		return false;
	}

	// 检查数据长度
	if (data.size() > DATA_LEN_MAX)
	{
		return false;
	}

	// 插入到任务队列
	m_tasksQueue.emplace(MsgType::WRITE, startAddress, data);
	m_taskCondition.notify_one();
	return true;
}

// 异步读数据
bool ModbusCppTcpClient::readRegistersAsync(const uint16_t startAddress, uint8_t dataLen)
{
	// 检查是否已初始化成功
	if (!m_connected)
	{
		std::cout << "not connected" << std::endl;
		return false;
	}

	// 检查数据长度
	if (dataLen > DATA_LEN_MAX)
	{
		std::cout << "datalen invalid" << std::endl;
		return false;
	}

	// 插入到任务队列
	m_tasksQueue.emplace(MsgType::READ, startAddress, dataLen);
	m_taskCondition.notify_one();
	return true;
}

// 异步读写数据
std::optional<std::vector<uint16_t>> ModbusCppTcpClient::writeAndReadRegistersAsync(const uint16_t writeStartAddress, const std::vector<uint16_t>& writeData, const uint16_t readStartAddress, const uint8_t readLen)
{
	// 检查是否已初始化成功
	if (!m_connected)
	{
		return std::nullopt;
	}

	// 检查数据长度
	if (writeData.size() > DATA_LEN_MAX || readLen > DATA_LEN_MAX)
	{
		return std::nullopt;
	}

	// 构造写数据
	for (int i = 0; i < writeData.size(); ++i)
	{
		m_writeBuffer[i] = writeData.at(i);
	}

	// 清空读缓存
	memset(m_readBuffer, 0, DATA_LEN_MAX);

	// 读写数据
	int _readNum = modbus_write_and_read_registers(m_modbusClient, writeStartAddress, static_cast<int>(writeData.size()), m_writeBuffer, readStartAddress, readLen, m_readBuffer);

	// 检查读取结果
	if (_readNum != readLen)
	{
		return std::nullopt;
	}

	// 组装读到的数据，返回结果
	std::vector<uint16_t> _data;
	for (int i = 0; i < readLen; ++i)
	{
		_data.push_back(m_readBuffer[i]);
	}
	return std::optional<std::vector<uint16_t>>(_data);
}

void ModbusCppTcpClient::checkConnectionStateThread()
{
	while (true)
	{
		// 因为检测 [未连接 -> 连接] 有点问题(当服务器，所以只检测 [连接 -> 断开] 的状态
		std::unique_lock<std::mutex> _lock(m_checkConnectionStateLock);
		m_checkConnectionStateCondition.wait(_lock, [this]() {
			return m_connected;
			});
		_lock.unlock();

		bool _b = isConnected();
		if (_b != m_connected)
		{
			m_checkConnectionStateLock.lock();
			m_connected = _b;
			m_checkConnectionStateLock.unlock();

			// 通知出去
			if (nullptr != m_connectionStateChangedCallback)
			{
				// std::cout << "connection state changed: " << _b;
				m_connectionStateChangedCallback(_b);
			}
		}

		//uint16_t dest;
		//int rc = modbus_read_registers(m_modbusClient, 0, 1, &dest);
		//if (rc == -1) {
		//    if (errno == ETIMEDOUT)
		//    {
		//        std::cout << "read timeout" << std::endl;
		//    }
		//    else if (errno == ECONNRESET || errno == ECONNABORTED)
		//    {
		//        std::cout << "disconnected" << std::endl;
		//    }
		//    else
		//    {
		//        std::cout << "other error: " << modbus_strerror(errno) << std::endl;
		//    }
		//}



		/*
		int error_code = errno;
		if (error_code == ECONNRESET || error_code == ETIMEDOUT || error_code == ECONNREFUSED || error_code == ENETUNREACH)
		{

		}
		//std::cout << modbus_get_socket(m_modbusClient) << std::endl;
		const int _readNum = modbus_read_registers(m_modbusClient, 0, 1, &a);
		std::cout << "error: " << errno << ", read num: " << _readNum << std::endl;
		*/
		std::this_thread::sleep_for(std::chrono::duration(std::chrono::milliseconds(500)));
	}
}

// 执行异步任务的线程
void ModbusCppTcpClient::processMsgThread()
{
	while (true)
	{
		std::unique_lock<std::mutex> _lock(m_taskLock);
		m_taskCondition.wait(_lock, [this] { return !m_tasksQueue.empty(); });

		ModbusMsg _msg = m_tasksQueue.front();
		m_tasksQueue.pop();
		_lock.unlock();

		switch (_msg.msgType)
		{
		case MsgType::WRITE:
		{
			bool _ret = writeRegistersSync(_msg.writeStartAddress, _msg.dataToWrite);
			if (_ret)
			{
				// 写入成功
			}
			else
			{
				// 写入失败
			}
			break;
		}
		case MsgType::READ:
		{
			const auto _result = readRegistersSync(_msg.readStartAddress, _msg.lenToRead);
			if (_result.has_value())
			{
				// 读取成功
				if (nullptr != m_receivedDataCallback)
				{
					m_receivedDataCallback(_msg.readStartAddress, _result.value());
				}
			}
			else
			{
				// 读取失败
				std::cout << "read failed" << std::endl;
			}
			break;
		}
		case MsgType::WRITE_AND_READ:
			break;
		default:
			// error
			std::cout << "error, unhandled msg type: " << (int)_msg.msgType << std::endl;
		}
	}
}
