# MCP 层说明

给这个 mini coding agent 加的**最小但真实**的 MCP（Model Context Protocol）支持。核心目的：演示"工具从进程外、运行时动态接入"——这正是 MCP 区别于进程内硬编码工具的本质。

## 文件

| 文件 | 角色 | 说明 |
|---|---|---|
| `mcp_server_demo.py` | **MCP Server**（独立进程） | 用 Python 写，通过 JSON-RPC 2.0 over stdio 暴露工具。用另一种语言，正是为了强调"工具提供方和 harness 完全解耦"。 |
| `mcp_client.hpp` | **MCP Client**（harness 侧） | 自包含头文件。用 fork/exec 拉起 server 子进程，建立双向管道，收发 JSON-RPC。 |
| `main.cc` | **入口与 harness 组装** | 读取环境变量，构造 Workspace/Model/MCP/MiniAgent，并执行一次任务。 |

## 协议

JSON-RPC 2.0，逐行 JSON（每条消息一行、`\n` 结尾），走子进程的 stdin/stdout：

- `initialize` — 握手
- `tools/list` — 返回工具清单（含 inputSchema），客户端据此**动态发现**
- `tools/call` — 执行工具，返回 `{"content":[{"type":"text","text":"..."}]}`

## 怎么跑

MCP 是**可选**的，由环境变量 `AGENT_MCP_CMD` 控制。不设置就只用本地工具，行为和以前完全一致。

```bash
g++ -std=c++17 -Wall -o mini_agent main.cc

export AGENT_API_KEY=sk-xxx
export AGENT_MCP_CMD="python3 mcp_server_demo.py"   # 关键：启用 MCP
./mini_agent ..
```

启动后会看到 `mcp: connected via [...]`，并且 `current_time` / `reverse_text` / `word_count` 三个工具会和本地工具一起被声明给模型（描述前带 `[via MCP]` 标记）。

## 两个接入点（harness 主体几乎零侵入）

1. **工具注册**（`build_tools()`）：`mcp_->discover()` 拿回的工具转成本地 `ToolSpec` 并入 `tools_`。从此它们和本地工具一视同仁——自动进入 `tools_to_json_schema` 声明给模型、走同样的 `run_tool` 闸门。

2. **dispatch 兜底**（`dispatch()`）：本地没匹配到的工具名，若来自 MCP 就转发给 `mcp_->call()`。这正是那个原本硬编码 if-else 链的"外部工具源兜底"。

## MCP vs Function Calling（别混）

- **Function Calling** 管"模型和 harness 之间怎么表达一次工具调用"（模型 ↔ harness）。
- **MCP** 管"工具从哪来、怎么接入 harness"（harness ↔ 外部工具源）。

典型链路：MCP 把外部工具发现进来 → 转成 Function Calling 的 JSON Schema → 喂给模型。两者是接力，不是二选一。

## 边界与简化（教学取舍）

- Server 用**逐行 JSON**而非官方 MCP 的 LSP 风格 `Content-Length` 分帧——协议语义一致，省去分帧解析。
- Client 的 JSON 解析是浅解析，覆盖扁平 `inputSchema`；嵌套参数需要正经 JSON 库。
- 调用是**同步阻塞**、单连接；不做并发、超时、重连。
- 未实现 MCP 的 resources/prompts 能力，只做了 tools。
