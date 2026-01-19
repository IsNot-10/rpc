#pragma once

#include <memory>
#include <functional>

class TimeStamp;
class TcpConnection;
class Buffer;

using TcpConnectionPtr=std::shared_ptr<TcpConnection>;
using ConnectionCallback=std::function<void(const TcpConnectionPtr&)>;
using CloseCallback=std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCallback=std::function<void(const TcpConnectionPtr&)>;
using HighWaterCallback=std::function<void(const TcpConnectionPtr&,size_t)>;
using MessageCallback=std::function<void(const TcpConnectionPtr&,Buffer*,TimeStamp)>;


