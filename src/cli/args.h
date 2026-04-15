#ifndef TITANBENCH_SRC_ARGS_H_
#define TITANBENCH_SRC_ARGS_H_

#include <string>

#include "core/config.h"

namespace titanbench {

// 版本号字符串
constexpr const char kVersion[] = "titanbench v0.1.0";

// 解析命令行参数
// argc: 参数个数
// argv: 参数数组
// config: 输出配置结构体
// error_message: 输出错误信息
// 返回: 解析是否成功
bool ParseArgs(int argc, char* argv[], Config* config, std::string* error_message);

// 验证配置的有效性
// config: 配置结构体（可能被修改）
// error_message: 输出错误信息
// 返回: 验证是否成功
bool ValidateConfig(Config* config, std::string* error_message);

// 构建帮助信息
// program_name: 程序名
// 返回: 帮助信息字符串
std::string BuildHelpMessage(const char* program_name);

// 构建版本信息
// 返回: 版本信息字符串
std::string BuildVersionMessage();

// 将协议枚举转换为字符串
// protocol: 协议枚举
// 返回: 协议字符串
std::string ProtocolToString(Protocol protocol);

// 从字符串解析协议
// protocol_text: 协议字符串
// protocol: 输出协议枚举
// 返回: 解析是否成功
bool ParseProtocol(const std::string& protocol_text, Protocol* protocol);

}  // namespace titanbench

#endif  // TITANBENCH_SRC_ARGS_H_
