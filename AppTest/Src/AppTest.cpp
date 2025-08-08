#include <iostream>
#include <chrono>
#include <thread>
#include "ModbusCppTcpClient.h"

// 测试常量
const std::string SERVER_HOST = "127.0.0.1";    // 服务器地址
const uint16_t SERVER_PORT = 502;               // 服务器端口
const int SLAVE_ID = 1;                         // 服务器ID
const int WRITE_START_ADDRESS = 1000;           // 写地址起始值
const int READ_START_ADDRESS = 0;               // 读地址起始值
const int WRITE_LEN = 120;                      // 写入数量
const int READ_LEN = 10;                        // 读取数量
const int TEST_TIMES = 10;                      // 测试次数
const int SLEEP_TIME = 60;

// 耗时统计
unsigned long long _requestTimes = 0;           // 请求(读/写)次数
double _writeDurationMax = 0;                   // 写入最大耗时
double _writeDurationMin = 999999;              // 写入最小耗时
double _readDurationMax = 0;                    // 读取最大耗时
double _readDurationMin = 999999;               // 读取最小耗时
double _writeAndReadDurationMax = 0;            // 读取最大耗时
double _writeAndReadDurationMin = 999999;       // 读取最小耗时

// 实例化对象
ModbusCppTcpClient _client;

// 测试写数据
bool testWirteData()
{
    // 构造数据
    std::vector<uint16_t> _data;
    for (int i = 0; i < WRITE_LEN; ++i)
    {
        _data.push_back(i);
    }

    // 开始计时
    auto _start = std::chrono::high_resolution_clock::now();

    // 写入数据
    bool _writeRet = _client.writeRegistersSync(WRITE_START_ADDRESS, _data);

    // 耗时统计
    auto _end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> _elapsed = _end - _start;
    double _duration = _elapsed.count();
    if (_duration > _writeDurationMax)
    {
        _writeDurationMax = _duration;
    }
    if (_duration < _writeDurationMin)
    {
        _writeDurationMin = _duration;
    }

    // 输出请求耗时和写入结果
    std::cout << std::format("第{:03d}次, 写入耗时: {:.3f} / [{:.3f} ~ {:.3f}] 毫秒, 写入{}\n", _requestTimes, _duration, _writeDurationMin, _writeDurationMax, (_writeRet ? "成功" : "失败"));
    return _writeRet;
}

// 测试读数据
bool testReadData()
{
    // 开始计时
    auto _start = std::chrono::high_resolution_clock::now();

    // 读取数据
    auto _result = _client.readRegistersSync(READ_START_ADDRESS, READ_LEN);

    // 统计耗时
    auto _end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> _elapsed = _end - _start;
    double _duration = _elapsed.count();
    if (_duration > _readDurationMax)
    {
        _readDurationMax = _duration;
    }
    if (_duration < _readDurationMin)
    {
        _readDurationMin = _duration;
    }

    // 输出请求耗时和读取结果
    std::cout << std::format("第{:03d}次, 读取耗时: {:.3f} / [{:.3f} ~ {:.3f}] 毫秒, 读取结果: ", _requestTimes, _duration, _readDurationMin, _readDurationMax);
    if (_result.has_value())
    {
        // 输出结果
        for (const auto& v : _result.value())
        {
            std::cout << v << ", ";
        }
        std::cout << std::endl;
    }
    else
    {
        std::cout << "读取失败" << std::endl;
        return false;
    }

    return true;
}

// 测试读写数据
bool testWriteAndReadData()
{
    // 构造数据
    std::vector<uint16_t> _data;
    for (int i = 0; i < WRITE_LEN; ++i)
    {
        _data.push_back(i);
    }

    // 开始计时
    auto _start = std::chrono::high_resolution_clock::now();

    // 写入并读取数据
    auto _result = _client.writeAndReadRegistersSync(WRITE_START_ADDRESS, _data, READ_START_ADDRESS, READ_LEN);

    // 耗时统计
    auto _end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> _elapsed = _end - _start;
    double _duration = _elapsed.count();
    if (_duration > _writeAndReadDurationMax)
    {
        _writeAndReadDurationMax = _duration;
    }
    if (_duration < _writeAndReadDurationMin)
    {
        _writeAndReadDurationMin = _duration;
    }

    // 输出请求耗时和读取结果
    std::cout << std::format("第{:03d}次, 读写耗时: {:.3f} / [{:.3f} ~ {:.3f}] 毫秒, 读取结果: ", _requestTimes, _duration, _writeAndReadDurationMin, _writeAndReadDurationMax);
    if (_result.has_value())
    {
        // 输出结果
        for (const auto& v : _result.value())
        {
            std::cout << v << ", ";
        }
        std::cout << std::endl;
    }
    else
    {
        std::cout << "读写失败" << std::endl;
        return false;
    }

    return true;
}

int main()
{
    // 连接服务器
    if (!_client.connectServer(SERVER_HOST, SERVER_PORT, SLAVE_ID))
    {
        std::cout << "连接失败" << std::endl;
        return 0;
    }
    std::cout << "连接成功" << std::endl;

    // 测试读写
    while (_requestTimes < TEST_TIMES)
    {
        // 请求次数 +1
        _requestTimes += 1;

        // 写数据
        bool _ret = testWirteData();
        if (!_ret)
        {
            break;
        }

        // 读数据
        _ret = testReadData();
        if (!_ret)
        {
            break;
        }

        // 读写数据
        _ret = testWriteAndReadData();
        if (!_ret)
        {
            break;
        }

        std::cout << std::endl;

        // 检查连接是否断开
        if (!_client.isConnected())
        {
            std::cout << "断开连接" << std::endl;
            break;
        }

        // 休眠(对某些PLC，读取间隔休眠，单次读取耗时会减小)
        std::this_thread::sleep_for(std::chrono::duration(std::chrono::milliseconds(SLEEP_TIME)));
    }
}

