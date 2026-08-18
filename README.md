# Mini Coding Agent Harness

这是一个用 C++17 写的最小 coding agent harness。它的目标不是堆生产级能力，而是把 agent 的核心结构摊开：

```text
组装 messages -> 调模型 -> 解析 tool_calls -> 执行工具 -> 记录结果 -> 继续循环
```

当前版本使用 OpenAI/DeepSeek 兼容的 Chat Completions + function calling 协议，支持原生 `messages` 数组、多工具调用、可选 MCP 工具接入，以及一个简单的交互式命令行入口。

## 快速运行

编译：

```bash
g++ -std=c++17 -Wall -Wextra -pedantic main.cc -o mini_agent
```

设置模型密钥：

```bash
export AGENT_API_KEY=sk-xxx
export AGENT_BASE_URL=https://api.deepseek.com/v1   # 可选，默认就是 DeepSeek
export AGENT_MODEL=deepseek-chat                    # 可选，默认低成本通用模型
```

启动：

```bash
./mini_agent .
```

进入交互后直接提问：

```text
mini-agent> 给我讲一下这个项目的整体架构
mini-agent> 读一下 main.cc 和 mini_agent.hpp 的关键逻辑
```

输入 `/exit` 或 `/quit` 退出。

## 项目结构

| 文件 | 作用 |
|---|---|
| `main.cc` | 程序入口：读取环境变量、构造 workspace/model/MCP/MiniAgent，进入交互循环 |
| `mini_agent.hpp` | harness 主体：agent 循环、工具注册、工具执行、历史记录、记忆、delegate |
| `model_client.hpp` | 模型边界：拼请求、发送 Chat Completions、解析 `tool_calls` 或最终文本 |
| `agent_types.hpp` | 纯数据结构：`ToolSpec` / `ToolCall` / `ModelReply` / `HistoryItem` / `Memory` |
| `workspace_context.hpp` | 工作区快照：当前目录、repo root、git branch、文件列表 |
| `json_utils.hpp` | 手写 JSON / shell 转义辅助 |
| `text_utils.hpp` | 通用文本与命令辅助：`clip` / `trim` / `now` / `sh_capture` |
| `mcp_client.hpp` | MCP client：启动外部 MCP server，发现工具，调用远端工具 |
| `mcp_server_demo.py` | 一个最小 MCP server 示例 |
| `MCP_README.md` | MCP 层专项说明 |

## 核心架构

整体分成三层：

```text
用户
  |
  v
MiniAgent harness
  |  function calling
  v
模型 API

MiniAgent harness
  |  JSON-RPC / MCP
  v
外部 MCP server
```

`MiniAgent::ask()` 是心脏。每次用户输入后，它会：

1. 把用户消息写入 `history_`。
2. 调用 `request_messages()` 生成原生 `messages` 数组。
3. 通过 `ModelClient::complete()` 调模型。
4. 如果模型返回 `tool_calls`，就记录 assistant 的工具调用，再逐个执行工具。
5. 把每个工具结果记录为 `role="tool"`，并用 `tool_call_id` 关联回对应调用。
6. 如果模型返回普通 `content`，就作为最终答案结束。
7. 如果工具步数耗尽，就禁用工具再请求模型基于已有结果总结。

## Function Calling 链路

本地工具先用 `ToolSpec` 表示：

```cpp
ToolSpec{
    "read_file",
    {{"path", "str"}, {"start", "int=1"}, {"end", "int=200"}},
    false,
    "Read a file by line range."
}
```

`tools_to_json_schema()` 会把它转换成模型 API 需要的 `tools` schema。模型如果要调工具，会返回：

```json
{
  "tool_calls": [
    {
      "id": "call_1",
      "type": "function",
      "function": {
        "name": "read_file",
        "arguments": "{\"path\":\"README.md\",\"start\":1,\"end\":80}"
      }
    }
  ]
}
```

`parse_tool_calls()` 会把这个结构转成：

```cpp
ToolCall{
    "call_1",
    "read_file",
    {{"path", "README.md"}, {"start", "1"}, {"end", "80"}}
}
```

下一轮发回模型时，历史会保留原生协议形态：

```json
{
  "role": "assistant",
  "content": null,
  "tool_calls": [...]
},
{
  "role": "tool",
  "tool_call_id": "call_1",
  "content": "工具执行结果..."
}
```

注意：远程模型返回的每个 `tool_call` 必须有 `id` 和 `name`。当前 harness 不会本地伪造 id；缺失时会返回协议错误。

## 内置工具

当前内置工具在 `MiniAgent::build_tools()` 里注册：

| 工具 | 作用 | 风险 |
|---|---|---|
| `list_files` | 列出工作区文件 | 低 |
| `read_file` | 按行读取文件 | 低 |
| `search` | 在工作区搜索文本 | 低 |
| `run_shell` | 在 repo root 执行 shell 命令 | 高，需要审批 |
| `write_file` | 写入文件 | 高，需要审批 |
| `patch_file` | 精确替换文件中的文本块 | 高，需要审批 |
| `delegate` | 派生只读子 agent 调查子任务 | 低，受深度限制 |

文件类工具都会经过 `resolve()` 路径沙箱，防止逃出当前 workspace。

## MCP 接入

MCP 是可选能力。不设置 `AGENT_MCP_CMD` 时，agent 只使用内置工具。

启用 demo MCP server：

```bash
export AGENT_MCP_CMD="python3 mcp_server_demo.py"
./mini_agent .
```

启动后，`MiniAgent::build_tools()` 会调用：

```cpp
mcp_->discover()
```

它会向外部 MCP server 发送 JSON-RPC：

```text
initialize
tools/list
```

拿到 MCP 工具清单后，harness 会做一层转换：

```text
MCP tools/list
  -> McpToolInfo
  -> ToolSpec
  -> function calling tools schema
  -> 发给模型
```

模型后续如果选择 MCP 工具，`dispatch()` 会通过：

```cpp
mcp_->call(name, args)
```

转发成 JSON-RPC `tools/call`，由 MCP server 执行真实逻辑。

更详细的 MCP 说明见 `MCP_README.md`。

## 设计取舍

这个项目刻意保持教学规模：

- 使用原生 `messages`，不再把历史压成一整段 prompt。
- 支持一轮多个 `tool_calls`。
- MCP 只实现 tools，不实现 resources/prompts。
- JSON 解析是手写浅解析，覆盖当前 demo 协议，不适合复杂嵌套 JSON。
- HTTP 调用通过 `curl` 子进程完成，简单直观；生产环境建议改成 libcurl/SDK 并加超时、重试、流式输出。
- transcript 和 memory 当前主要在内存里，尚未实现完整落盘 resume。

## 环境变量

| 变量 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `AGENT_API_KEY` | 是 | 无 | 模型 API key |
| `AGENT_BASE_URL` | 否 | `https://api.deepseek.com/v1` | OpenAI 兼容 API 根地址 |
| `AGENT_MODEL` | 否 | `deepseek-chat` | 模型名 |
| `AGENT_MCP_CMD` | 否 | 空 | 外部 MCP server 启动命令 |

## 常见问题

### 为什么 MCP 工具不能直接原样发给模型？

因为 MCP 和 function calling 是两套协议。MCP 是 harness 和外部工具服务之间的 JSON-RPC 协议；function calling 是 harness 和模型之间的工具协议。两者表达的语义接近，但 wire format 不同，所以 harness 必须做适配转换。

### 为什么工具参数都转成 `std::string`？

为了保持工具分发层简单。模型 schema 可以声明 integer/string，但进入本地工具层后统一变成 `map<string, string>`，需要整数时再 `std::stoi`。

### 为什么没有本地补 `tool_call.id`？

因为原生 function calling 协议要求模型返回 `tool_call.id`，后续 `tool` 消息必须用这个 id 回指。缺 id 说明模型响应不合规，当前实现会报错，不伪造。
