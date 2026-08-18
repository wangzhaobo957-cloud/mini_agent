// agent_types.hpp
// ---------------------------------------------------------------------------
// 数据结构：工具元数据 / 模型回复 / 历史条目 / 蒸馏记忆。
// （function calling 下，工具元数据既用于展示，也用于生成发给 API 的 JSON Schema）
// 这些是 harness 与模型客户端之间流转的纯数据类型，无行为，故单独成文件。
// ---------------------------------------------------------------------------
#ifndef AGENT_TYPES_HPP
#define AGENT_TYPES_HPP

#include <map>
#include <string>
#include <utility>
#include <vector>

// ToolSpec: 一个工具的元数据（名字、参数模式、是否危险、描述）。
// schema 里每个参数的类型串形如 "str" / "str='.'" / "int=1"：带 '=' 表示有默认值(可选)，否则必填。
struct ToolSpec {
    std::string name;
    std::vector<std::pair<std::string, std::string>> schema;  // 保留声明顺序
    bool risky;
    std::string description;
};

// ToolCall: 模型请求的一次具体工具调用，带 id/name/args 三元组。
// {
//   "id": "call_1",
//   "type": "function",
//   "function": {
//     "name": "read_file",
//     "arguments": "{\"path\":\"README.md\",\"start\":1,\"end\":80}"
//   }
// }
// ToolCall{
//     "call_1",
//     "read_file",
//     {
//         {"path", "README.md"},
//         {"start", "1"},
//         {"end", "80"}
//     }
// }
struct ToolCall {
    std::string id;
    std::string name;
    std::map<std::string, std::string> args;
};

// ModelReply: 模型一轮回复的结构化结果 —— function calling 的核心。
// 要么请求调用一批工具(is_tool_call=true, tool_calls)，要么给出最终文本(content)。
// 对比文本协议：过去要靠 parse() 从自由文本里猜；现在模型直接给结构，无需解析、无需容错。
//模型回复结构化结果
struct ModelReply {
    bool is_tool_call = false;
    std::vector<ToolCall> tool_calls;//可能有多轮工具调用
    std::string content;   // 非工具调用时的最终答案（或错误信息）
};

// HistoryItem: 一条原生 chat message（system/user/assistant/tool）及其工具调用元数据。
struct HistoryItem {
    std::string role;                          // "system" / "user" / "assistant" / "tool"
    std::string name;                          // assistant 发起工具调用或 tool 返回结果时的工具名
    std::map<std::string, std::string> args;   // 工具参数
    std::string content;                       // 文本内容 / 工具返回
    std::string tool_call_id;                  // 关联 assistant.tool_calls 与 tool 结果
    std::string created_at;
    std::vector<ToolCall> tool_calls;          // 当 role=="assistant" 时可携带一批 tool_calls
};

// Memory: 蒸馏后的工作记忆，只保留「当前任务 + 碰过的文件 + 最近几条笔记」。
struct Memory {
    std::string task;
    std::vector<std::string> files;// 碰过的文件路径
    std::vector<std::string> notes;// 最近几条笔记
};

#endif  // AGENT_TYPES_HPP
