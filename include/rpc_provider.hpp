#ifndef __RPC_SERVER_H__
#define __RPC_SERVER_H__

#include <map>
#include <any>
#include <stdexcept>
#include <cstring>
#include <string>
#include <iostream>
#include <functional>
#include <unordered_map>
#include <string>
#include <any>
#include <vector>
#include <tuple>
#include <typeinfo>
#include <cxxabi.h>
#include <array>
#include <list>
#include <memory>
#include <algorithm>
#include <chrono>
#include <optional>
#include <variant>
#include <future>
#include <thread>
#include <sstream>
#include <cctype>
#include "json.hpp"

namespace rpc {
    // 自定义异常类
    class RpcException : public std::runtime_error {
    public:
        enum class ErrorType {
            FUNCTION_NOT_FOUND,
            TYPE_MISMATCH,
            ARGUMENT_ERROR,
            TIMEOUT_ERROR
        };

        RpcException(ErrorType type, const std::string& message)
            : std::runtime_error(message), type_(type) {}

        ErrorType type() const { return type_; }

    private:
        ErrorType type_;
    };

    // 获取可读的类型名称
    inline std::string demangle(const char* name) {
        int status;
        char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
        if (status == 0) {
            std::string result(demangled);
            free(demangled);
            return result;
        }
        return name;
    }

    namespace detail {
        // 类型转换辅助函数
        template<typename T>
        std::any json_to_any(const nlohmann::json& j) {
            if constexpr (std::is_same_v<T, int>) {
                return std::make_any<int>(j.get<int>());
            } else if constexpr (std::is_same_v<T, long>) {
                return std::make_any<long>(j.get<long>());
            } else if constexpr (std::is_same_v<T, float>) {
                return std::make_any<float>(j.get<float>());
            } else if constexpr (std::is_same_v<T, double>) {
                return std::make_any<double>(j.get<double>());
            } else if constexpr (std::is_same_v<T, bool>) {
                return std::make_any<bool>(j.get<bool>());
            } else if constexpr (std::is_same_v<T, std::string>) {
                return std::make_any<std::string>(j.get<std::string>());
            } else if constexpr (std::is_same_v<T, std::vector<int>>) {
                return std::make_any<std::vector<int>>(j.get<std::vector<int>>());
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                return std::make_any<std::vector<std::string>>(j.get<std::vector<std::string>>());
            } else {
                throw RpcException(
                    RpcException::ErrorType::TYPE_MISMATCH,
                    "Unsupported type conversion from JSON"
                );
            }
        }

        // 类型转换函数
        std::any convert_json_to_any(const nlohmann::json& j, const std::type_info& type) {
            if (type == typeid(int)) {
                return json_to_any<int>(j);
            } else if (type == typeid(long)) {
                return json_to_any<long>(j);
            } else if (type == typeid(float)) {
                return json_to_any<float>(j);
            } else if (type == typeid(double)) {
                return json_to_any<double>(j);
            } else if (type == typeid(bool)) {
                return json_to_any<bool>(j);
            } else if (type == typeid(std::string)) {
                return json_to_any<std::string>(j);
            } else if (type == typeid(std::vector<int>)) {
                return json_to_any<std::vector<int>>(j);
            } else if (type == typeid(std::vector<std::string>)) {
                return json_to_any<std::vector<std::string>>(j);
            } else {
                throw RpcException(
                    RpcException::ErrorType::TYPE_MISMATCH,
                    "Unsupported type: " + demangle(type.name())
                );
            }
        }

        // 分割字符串
        inline std::vector<std::string> split(const std::string& s, char delimiter) {
            std::vector<std::string> tokens;
            std::string token;
            std::istringstream tokenStream(s);
            while (std::getline(tokenStream, token, delimiter)) {
                // 去除空格
                token.erase(0, token.find_first_not_of(" \t\n\r\f\v"));
                token.erase(token.find_last_not_of(" \t\n\r\f\v") + 1);
                if (!token.empty()) {
                    tokens.push_back(token);
                }
            }
            return tokens;
        }
    }

    class RpcProvider {
    public:
        // 存储函数签名信息的结构
        struct FunctionInfo {
            const std::type_info* returnType;
            std::vector<const std::type_info*> paramTypes;
            std::vector<std::string> paramNames;
            std::function<std::any(const std::vector<std::any>&)> func;
            std::chrono::milliseconds timeout{5000};
        };

        // 注册函数
        template <typename Ret, typename... Args>
        void register_function(const std::string& name, 
                            std::function<Ret(Args...)> func,
                            const std::vector<std::string>& param_names,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
            // 编译时类型检查
            static_assert((std::is_copy_constructible_v<Args> && ...), 
                        "All argument types must be copy constructible");
            static_assert(std::is_copy_constructible_v<Ret> || std::is_void_v<Ret>, 
                        "Return type must be copy constructible");

            // 检查参数名称数量是否匹配
            if (param_names.size() != sizeof...(Args)) {
                throw RpcException(
                    RpcException::ErrorType::ARGUMENT_ERROR,
                    "Parameter names count mismatch"
                );
            }

            FunctionInfo info;
            info.returnType = &typeid(Ret);
            info.paramTypes = {&typeid(Args)...};
            info.paramNames = param_names;
            info.timeout = timeout;
            info.func = [func](const std::vector<std::any>& args) -> std::any {
                if (args.size() != sizeof...(Args)) {
                    throw RpcException(
                        RpcException::ErrorType::ARGUMENT_ERROR,
                        "Arguments count mismatch"
                    );
                }
                return call_impl<Ret, Args...>(func, args, std::make_index_sequence<sizeof...(Args)>{});
            };
            functions[name] = std::move(info);
        }

        // 通过 JSON 进行命名参数调用
        template <typename Ret>
        Ret call_function_named(const std::string& name, const nlohmann::json& named_args) {
            auto it = functions.find(name);
            if (it == functions.end()) {
                throw RpcException(
                    RpcException::ErrorType::FUNCTION_NOT_FOUND,
                    "Function not found: " + name
                );
            }

            const auto& info = it->second;

            // 检查返回类型
            if (*info.returnType != typeid(Ret)) {
                throw RpcException(
                    RpcException::ErrorType::TYPE_MISMATCH,
                    "Return type mismatch. Expected " + 
                    demangle(info.returnType->name()) + 
                    " but got " + demangle(typeid(Ret).name())
                );
            }

            // 按照注册时的参数顺序重新排列参数
            std::vector<std::any> ordered_args;
            ordered_args.reserve(info.paramNames.size());

            for (size_t i = 0; i < info.paramNames.size(); ++i) {
                const auto& param_name = info.paramNames[i];
                
                // 检查参数是否存在
                if (!named_args.contains(param_name)) {
                    throw RpcException(
                        RpcException::ErrorType::ARGUMENT_ERROR,
                        "Missing argument: " + param_name
                    );
                }

                try {
                    // 转换参数类型
                    ordered_args.push_back(
                        detail::convert_json_to_any(named_args[param_name], *info.paramTypes[i])
                    );
                } catch (const nlohmann::json::exception& e) {
                    throw RpcException(
                        RpcException::ErrorType::TYPE_MISMATCH,
                        "JSON conversion error for parameter '" + param_name + "': " + e.what()
                    );
                }
            }

            try {
                // 创建异步任务
                auto future = std::async(std::launch::async, [&]() {
                    return info.func(ordered_args);
                });

                // 等待结果，带超时
                if (future.wait_for(info.timeout) == std::future_status::timeout) {
                    throw RpcException(
                        RpcException::ErrorType::TIMEOUT_ERROR,
                        "Function call timed out: " + name
                    );
                }

                if constexpr (std::is_void_v<Ret>) {
                    future.get();
                    return;
                } else {
                    return std::any_cast<Ret>(future.get());
                }
            } catch (const std::bad_any_cast& e) {
                throw RpcException(
                    RpcException::ErrorType::TYPE_MISMATCH,
                    "Type conversion error: " + std::string(e.what())
                );
            }
        }

        // 设置函数超时时间
        void set_timeout(const std::string& name, std::chrono::milliseconds timeout) {
            auto it = functions.find(name);
            if (it != functions.end()) {
                it->second.timeout = timeout;
            }
        }

    private:
        // 存储注册的函数
        std::unordered_map<std::string, FunctionInfo> functions;

        // 辅助函数：展开参数并调用函数
        template<typename Ret, typename... Args, std::size_t... I>
        static std::any call_impl(const std::function<Ret(Args...)>& func,
                                const std::vector<std::any>& args,
                                std::index_sequence<I...>) {
            if constexpr (std::is_void_v<Ret>) {
                func(std::any_cast<std::decay_t<Args>>(args[I])...);
                return std::any();
            } else {
                return func(std::any_cast<std::decay_t<Args>>(args[I])...);
            }
        }
    };
}

// 用于自动获取参数名称的宏
#define PARAM_NAMES(...) rpc::detail::split(#__VA_ARGS__, ',')

// 用于注册函数的宏
#define REGISTER_FUNCTION(server, name, func, ...) \
    server.register_function(name, std::function(func), PARAM_NAMES(__VA_ARGS__))

#endif