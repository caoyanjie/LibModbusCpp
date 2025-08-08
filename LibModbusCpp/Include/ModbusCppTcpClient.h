#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <tuple>
#include <optional>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>

#if defined(_MSC_VER)
    # if defined(DLLBUILD)
    #  define MODBUSCPP_API __declspec(dllexport)
    # else
    #  define MODBUSCPP_API __declspec(dllimport)
    # endif
#else
    # define MODBUSCPP_API
#endif


typedef struct _modbus modbus_t;

class MODBUSCPP_API ModbusCppTcpClient
{
public:
    ModbusCppTcpClient();
	~ModbusCppTcpClient();

    // 设置参数
    bool setTimeout(uint64_t msec);
    void setRetries(const uint8_t retries);

    // 设置回调
    void setRequestFailedCallback(const std::function<void ()> callback);
    void setReceivedDataCallback(const std::function<void (const uint16_t startAddress, const std::vector<uint16_t> data)> callback);
    void setConnectionStateChangedCallback(const std::function<void (bool connected)> callback);

    // 连接服务器
    bool connectServer(const std::string &serverHost, const uint16_t serverPort, const int slaveId);
    void disconnectServer();
	bool isConnected();

    // 同步读写
    bool writeRegistersSync(const uint16_t startAddress, const std::vector<uint16_t> &data);
    std::optional<std::vector<uint16_t>> readRegistersSync(const uint16_t startAddress, const uint8_t dataLen);
    std::optional<std::vector<uint16_t>> writeAndReadRegistersSync(const uint16_t writeStartAddress, const std::vector<uint16_t>& writeData, const uint16_t readStartAddress, const uint8_t readLen);

    // 异步读写
    bool writeRegistersAsync(const uint16_t startAddress, const std::vector<uint16_t> &data);
    bool readRegistersAsync(const uint16_t startAddress, const uint8_t dataLen);
    std::optional<std::vector<uint16_t>> writeAndReadRegistersAsync(const uint16_t writeStartAddress, const std::vector<uint16_t>& writeData, const uint16_t readStartAddress, const uint8_t readLen);

private:
    void processMsgThread();
    void checkConnectionStateThread();

    enum class MsgType
    {
        NONE,
        WRITE,
        READ,
        WRITE_AND_READ
    };

    struct ModbusMsg
    {
        // 写数据
        ModbusMsg(const MsgType type, const uint16_t startAddress, std::vector<uint16_t> &&data)
            : msgType(type), writeStartAddress(startAddress), dataToWrite(std::move(data)) {}

        ModbusMsg(const MsgType type, const uint16_t startAddress, const std::vector<uint16_t> &data)
            : msgType(type), writeStartAddress(startAddress), dataToWrite(data) {}

        // 读数据
        ModbusMsg(const MsgType type, const uint16_t startAddress, const uint8_t len)
            : msgType(type), readStartAddress(startAddress), lenToRead(len) {}

        MsgType                 msgType = MsgType::NONE;

        uint16_t                writeStartAddress = 0;
        std::vector<uint16_t>   dataToWrite;

        uint16_t                readStartAddress = 0;
        uint8_t                 lenToRead = 0;
    };

    static const uint8_t    DATA_LEN_MAX = 125;

    bool                    m_connected;
    std::mutex              m_checkConnectionStateLock;
    std::condition_variable m_checkConnectionStateCondition;

    uint16_t                m_readBuffer[DATA_LEN_MAX];
    uint16_t                m_writeBuffer[DATA_LEN_MAX];

    modbus_t                *m_modbusClient;
    int                     m_timeoutSec;
    int                     m_timeoutUsec;
    int                     m_retries;
    std::queue<ModbusMsg>   m_tasksQueue;
    std::mutex              m_taskLock;
    std::condition_variable m_taskCondition;

    std::function<void ()> m_requestFailedCallback;
    std::function<void (const uint16_t startAddress, const std::vector<uint16_t> &data)> m_receivedDataCallback;
    std::function<void (bool connected)> m_connectionStateChangedCallback;

    std::mutex m_lockTest;
};

