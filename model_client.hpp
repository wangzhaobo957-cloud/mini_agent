// model_client.hpp
// ---------------------------------------------------------------------------
// 模型客户端接口 —— harness 与「大脑」之间唯一的边界。
// 采用 function calling：工具以 JSON Schema 通过 API 的 tools 参数声明给模型，
// 模型直接返回结构化的 tool_calls（要调工具）或最终文本（要收尾）。
//   - ModelClient       接口，complete() 是唯一方法
//   - FakeModelClient   脚本化「结构化回复」，离线跑通整条循环
//   - RemoteModelClient 调 OpenAI 兼容接口（/v1/chat/completions + tools）
// 附带 tools_to_json_schema：把 ToolSpec 列表转成 tools 数组，取代旧版 prompt 里的大白话工具清单。
// ---------------------------------------------------------------------------
#ifndef MODEL_CLIENT_HPP
#define MODEL_CLIENT_HPP

#include <deque>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "agent_types.hpp"  // ToolSpec / ModelReply
#include "json_utils.hpp"    // json_quote / shell_quote / extract_json_string / parse_flat_json_object
#include "text_utils.hpp"    // clip / sh_capture

// tools_to_json_schema: 把 ToolSpec 列表转成 OpenAI function calling 要求的 tools 数组(JSON Schema)。
// 这一步取代了旧版在 prompt 文本里用大白话列工具——现在以结构化契约的方式声明给模型。
inline std::string tools_to_json_schema(const std::vector<ToolSpec> &tools) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < tools.size(); ++i) {
        const ToolSpec &t = tools[i];
        std::vector<std::string> required;
        o << "{\"type\":\"function\",\"function\":{"
          << "\"name\":" << json_quote(t.name) << ","
          << "\"description\":" << json_quote(t.description) << ","
          << "\"parameters\":{\"type\":\"object\",\"properties\":{";
        for (size_t j = 0; j < t.schema.size(); ++j) {
            const auto &pr = t.schema[j];
            // "int..." -> integer，其余 -> string；不含 '=' 视为必填参数。
            std::string type = pr.second.rfind("int", 0) == 0 ? "integer" : "string";
            if (pr.second.find('=') == std::string::npos) required.push_back(pr.first);
            o << json_quote(pr.first) << ":{\"type\":\"" << type << "\"}";
            if (j + 1 < t.schema.size()) o << ",";
        }
        o << "},\"required\":[";
        for (size_t r = 0; r < required.size(); ++r) {
            o << json_quote(required[r]);
            if (r + 1 < required.size()) o << ",";
        }
        o << "]}}}";
        if (i + 1 < tools.size()) o << ",";
    }
    o << "]";
    return o.str();
}

// ===========================================================================
// 模型客户端接口 —— harness 与「大脑」之间唯一的边界
// ===========================================================================
struct ModelClient {
    virtual ~ModelClient() = default;
    // complete: 输入完整 prompt + 可用工具定义，返回结构化回复（工具调用 或 最终文本）。
    // 工具通过 tools 参数以 JSON Schema 声明给模型，模型直接返回结构，无需 prompt 教格式。
    virtual ModelReply complete(const std::string &prompt,
                                const std::vector<ToolSpec> &tools,
                                int max_new_tokens) = 0;
};

// FakeModelClient: 用预设的「结构化回复」按顺序吐出，让整条 agent 循环无需真实模型也能跑通。
// 每个 ModelReply 要么是工具调用、要么是最终文本——直接就是结构，无需再解析。
struct FakeModelClient : ModelClient {
    std::deque<ModelReply> outputs;
    std::vector<std::string> seen_prompts;  // 记录每轮实际喂进去的 prompt，便于观察

    explicit FakeModelClient(std::vector<ModelReply> outs)
        : outputs(outs.begin(), outs.end()) {}

    // complete: 记录本轮 prompt，然后弹出脚本里的下一条结构化回复（忽略 tools 参数）。
    ModelReply complete(const std::string &prompt, const std::vector<ToolSpec> &, int) override {
        seen_prompts.push_back(prompt);
        if (outputs.empty()) {
            ModelReply r; r.content = "fake model ran out of outputs"; return r;
        }
        ModelReply out = outputs.front();
        outputs.pop_front();
        return out;
    }
};

// RemoteModelClient: 调用「OpenAI 兼容」的远程 Chat Completions API，并启用 function calling。
// 工具经 tools 参数以 JSON Schema 声明；模型若要调工具则返回 tool_calls，否则返回 message.content。
// 与文本协议版的根本区别：不在 prompt 里教 <tool>/<final>，也不用正则解析——直接读结构化字段。
struct RemoteModelClient : ModelClient {
    std::string base_url;   // API 根地址，如 https://api.openai.com/v1
    std::string model;      // 模型名，如 gpt-4o-mini / deepseek-chat
    std::string api_key;    // 鉴权用的密钥（从环境变量读入，不硬编码）
    double temperature;     // 采样温度，越低越稳定

    // 构造函数：绑定服务地址、模型、密钥与温度（对应 CLI/环境变量）。
    RemoteModelClient(std::string base_url, std::string model, std::string api_key,
                      double temperature = 0.2)
        : base_url(std::move(base_url)), model(std::move(model)),
          api_key(std::move(api_key)), temperature(temperature) {}

    // complete: 带上 tools 定义发请求；解析响应——有 tool_calls 就返回工具调用，否则返回最终文本。
    ModelReply complete(const std::string &prompt, const std::vector<ToolSpec> &tools,
                        int max_new_tokens) override {
        // 1) 拼请求体：单条 user 消息 + tools 定义。tool_choice=auto 让模型自行决定是否调工具。
        std::string body = std::string("{")
            + "\"model\":" + json_quote(model) + ","
            + "\"temperature\":" + std::to_string(temperature) + ","
            + "\"max_tokens\":" + std::to_string(max_new_tokens) + ","
            + "\"tools\":" + tools_to_json_schema(tools) + ","
            + "\"tool_choice\":\"auto\","
            + "\"messages\":[{\"role\":\"user\",\"content\":" + json_quote(prompt) + "}]"
            + "}";

        // 2) 发 HTTPS POST，带上 Authorization: Bearer <key>。演示用 curl 子进程；生产建议 libcurl。
        std::string url = base_url + "/chat/completions";
        std::string cmd = "curl -s --fail-with-body -X POST " + shell_quote(url)
            + " -H 'Content-Type: application/json'"
            + " -H " + shell_quote("Authorization: Bearer " + api_key)
            + " -d " + shell_quote(body);
        std::string raw = sh_capture(cmd);

        // 3) 解析响应。先看有没有 tool_calls：有则取 function.name + arguments 组成工具调用。
        ModelReply reply;
        size_t tc = raw.find("\"tool_calls\"");
        if (tc != std::string::npos) {
            // function.name 是 tool_calls 之后第一个 "name" 字段。
            size_t np = raw.find("\"name\"", tc);
            reply.tool_name = extract_json_string(raw.substr(np == std::string::npos ? tc : np), "name");
            // arguments 是一段被转义的 JSON 字符串，取出后反转义再解析成参数 map。
            std::string args_json = extract_json_string(raw.substr(tc), "arguments");
            reply.args = parse_flat_json_object(args_json);
            reply.is_tool_call = !reply.tool_name.empty();
            if (reply.is_tool_call) return reply;
        }
        // 否则取 message.content 作为最终答案；都取不到则回传错误信息。
        std::string content = extract_json_string(raw, "content");
        if (content.empty()) {
            std::string err = extract_json_string(raw, "message");
            reply.content = "remote model error: " + (err.empty() ? clip(raw, 300) : err);
        } else {
            reply.content = content;
        }
        return reply;
    }
};

#endif  // MODEL_CLIENT_HPP
