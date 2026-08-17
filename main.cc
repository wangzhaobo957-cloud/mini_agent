// main.cc
// ---------------------------------------------------------------------------
// 一个「最小 harness」的 C++ 教学实现，对照 rasbt/mini-coding-agent 的 Python 版本。
// 目标：用最少的代码把「agent = 在循环里反复(组装messages -> 调模型 -> 解析 -> 执行工具 -> 记录)」
// 这件事显式地摊开。仅依赖 C++17 标准库，g++ -std=c++17 main.cc 即可编译。
//
// 六大组件 -> 代码符号（及所在文件）的映射（与 Python 版一一对应）：
//   1) Live Repo Context            -> WorkspaceContext              (workspace_context.hpp)
//   2) Prompt Shape & Cache Reuse   -> build_prefix / memory_text / request_messages   (mini_agent.hpp)
//   3) Tools + Validation + Perms   -> build_tools / run_tool / approve / tool_*  (mini_agent.hpp)
//   4) Context Reduction            -> clip / history_text           (text_utils.hpp / mini_agent.hpp)
//   5) Transcript + Memory + Resume -> history_ / memory_ / record / remember    (mini_agent.hpp)
//   6) Delegation & Bounded Subagent-> tool_delegate                 (mini_agent.hpp)
//
// 数据结构与「大脑」边界：
//   - 纯数据类型（ToolSpec/ModelReply/HistoryItem/Memory）  -> agent_types.hpp
//   - 手写 JSON/shell 辅助                                  -> json_utils.hpp
//   - 通用小工具（clip/trim/now/sh_capture）                -> text_utils.hpp
//   - 模型客户端接口与实现（Fake/Remote）+ tools 声明        -> model_client.hpp
//
// 模型端(ModelClient)是一个接口，采用 function calling：工具以 JSON Schema 通过 API 的 tools 参数
// 声明给模型，模型直接返回结构化的 tool_calls（要调工具）或最终文本（要收尾）。因此 prompt 里不再教
// <tool>/<final> 格式，也不再需要正则容错解析。FakeModelClient 用脚本化的「结构化回复」离线跑通循环；
// RemoteModelClient 调用 OpenAI 兼容接口（/v1/chat/completions + tools）。
//
// 本文件只负责「组装」：读环境变量 -> 建工作区 -> 造模型/可选 MCP -> 构造 MiniAgent -> 进入交互循环。
// 各类的定义已按组件拆分到上述头文件中，避免单文件过长、便于分模块阅读。
// ---------------------------------------------------------------------------

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mcp_client.hpp"         // mcp::McpClient：可选的外部工具源
#include "mini_agent.hpp"         // MiniAgent：harness 本体（间接引入其余头文件）
#include "model_client.hpp"       // RemoteModelClient
#include "workspace_context.hpp"  // WorkspaceContext

// ===========================================================================
// main —— 组装 workspace + 远程模型，让 agent 进入交互式问答循环
// ===========================================================================

// env_or: 读取环境变量，未设置时返回默认值（集中管理配置，避免把 key 硬编码进源码）。
static std::string env_or(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// main: 从环境变量读取远程模型配置（base_url/model/api_key），构造 agent 并持续读取用户请求。
int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    //建立工作区快照，记录当前目录下的所有文件，工作区快照
    WorkspaceContext ws = WorkspaceContext::build(dir);

    // 配置来自环境变量：
    //   AGENT_API_KEY   必填，远程模型的密钥（不写进源码/命令行，避免泄露）
    //   AGENT_BASE_URL  可选，OpenAI 兼容 API 根地址，默认 DeepSeek
    //   AGENT_MODEL     可选，模型名，默认 DeepSeek 最便宜的通用模型 deepseek-chat
    std::string api_key  = env_or("AGENT_API_KEY", "");
    std::string base_url = env_or("AGENT_BASE_URL", "https://api.deepseek.com/v1");
    std::string model_name = env_or("AGENT_MODEL", "deepseek-chat");

    if (api_key.empty()) {//未设置环境变量 AGENT_API_KEY
        std::cerr << "error: 环境变量 AGENT_API_KEY 未设置。\n"
                  << "用法示例:\n"
                  << "  export AGENT_API_KEY=sk-xxx\n"
                  << "  export AGENT_BASE_URL=https://api.deepseek.com/v1 # 可选\n"
                  << "  export AGENT_MODEL=deepseek-chat                  # 可选\n"
                  << "  ./mini_agent [workspace_dir]\n";
        return 1;
    }

    // 真实远程模型：换脑不换 harness——下面 MiniAgent 及其循环一个字都不用改。
    RemoteModelClient model(base_url, model_name, api_key);

    // MCP（可选）：若设置了 AGENT_MCP_CMD，则据此拉起一个外部 MCP server 子进程，
    //   把它暴露的工具动态接入 agent。例：
    //     export AGENT_MCP_CMD="python3 mcp_server_demo.py"
    //   不设置则 mcp_ptr 为 nullptr，agent 只用本地工具，行为与之前完全一致。
    mcp::McpClient mcp;
    mcp::McpClient *mcp_ptr = nullptr;
    std::string mcp_cmd = env_or("AGENT_MCP_CMD", "");
    if (!mcp_cmd.empty()) {
        std::vector<std::string> argv;  // 按空格切分命令（demo 够用，不处理引号）
        std::istringstream iss(mcp_cmd);
        for (std::string tok; iss >> tok;) argv.push_back(tok);
        if (mcp.start(argv)) {
            mcp_ptr = &mcp;
            std::cout << "mcp: connected via [" << mcp_cmd << "]\n";
        } else {
            std::cerr << "mcp: 启动失败: " << mcp.last_error() << "（将仅用本地工具）\n";
        }
    }

    // approval=ask：真实模型可能提出危险操作，默认逐个确认（trusted 场景可改 auto）。
    MiniAgent agent(model, ws, /*approval=*/"ask", /*max_steps=*/10,
                    /*depth=*/0, /*max_depth=*/1, /*read_only=*/false, /*mcp=*/mcp_ptr);

    std::cout << "=== MINI CODING AGENT (C++) ===\n"
              << "workspace: " << ws.cwd << " | branch: " << ws.branch << "\n"
              << "model: " << model.model << " @ " << base_url << "\n"
              << "输入你的问题；输入 /exit 或 /quit 退出。\n\n";

    std::string question;
    while (true) {
        std::cout << "mini-agent> " << std::flush;
        if (!std::getline(std::cin, question)) break;
        question = trim(question);
        if (question.empty()) continue;
        if (question == "/exit" || question == "/quit") break;

        std::string answer = agent.ask(question);
        std::cout << "\n[final answer] " << answer << "\n\n";
    }
    return 0;
}
