#!/usr/bin/env python3
# mcp_server_demo.py
# ---------------------------------------------------------------------------
# 一个「最小但真实」的 MCP Server（独立进程），用 JSON-RPC 2.0 over stdio 通信。
#
# 为什么用 Python 单独一个文件、而不是写进 C++ 主程序里？
#   这正是 MCP 的灵魂：工具提供方是一个**独立进程**，和 harness 完全解耦——
#   语言可以不同、可以是任何第三方进程。harness（C++ 侧的 McpClient）只通过
#   标准输入/输出交换 JSON-RPC 报文，运行时动态发现它暴露了哪些工具。
#
# 协议（MCP stdio transport 的简化版，逐行 JSON）：
#   - 每条消息是一行 JSON（不含内嵌换行），以 '\n' 结尾。
#   - 请求：{"jsonrpc":"2.0","id":N,"method":"...","params":{...}}
#   - 响应：{"jsonrpc":"2.0","id":N,"result":{...}}  或  {..."error":{...}}
#
# 支持的方法：
#   initialize  -> 握手，返回 serverInfo
#   tools/list  -> 返回本 server 暴露的工具清单（含 inputSchema）
#   tools/call  -> 执行一个工具，返回 {"content":[{"type":"text","text":"..."}]}
# ---------------------------------------------------------------------------

import sys
import json
import os
import datetime


# TOOLS: 本 server 对外暴露的工具目录。每个工具含 name/description/inputSchema，
# inputSchema 用 JSON Schema 描述参数——这正是 harness 侧动态发现工具的依据。
TOOLS = [
    {
        "name": "current_time",
        "description": "Return the current local date and time (from the MCP server process).",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "reverse_text",
        "description": "Reverse the given text. A pure, deterministic demo tool.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "word_count",
        "description": "Count lines, words and characters of a text file at the given path.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
        },
    },
]


# tool_current_time: 返回 server 进程本地的当前时间。args 未用（本工具无参数）。
def tool_current_time(args):
    del args
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# tool_reverse_text: 把入参 text 反转后返回（纯函数，便于确定性验证）。
def tool_reverse_text(args):
    return (args.get("text") or "")[::-1]


# tool_word_count: 读取指定文件，统计其行数/词数/字符数。
def tool_word_count(args):
    path = args.get("path") or ""
    if not path or not os.path.isfile(path):
        raise ValueError("path is not a file: %r" % path)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        data = f.read()
    lines = data.count("\n") + (0 if data.endswith("\n") or not data else 1)
    words = len(data.split())
    chars = len(data)
    return "lines=%d words=%d chars=%d" % (lines, words, chars)


HANDLERS = {
    "current_time": tool_current_time,
    "reverse_text": tool_reverse_text,
    "word_count": tool_word_count,
}


# handle: 根据 JSON-RPC method 分发，返回 (result, error) 二元组之一。
def handle(method, params):
    if method == "initialize":
        return {"protocolVersion": "demo-1", "serverInfo": {"name": "mini-mcp-demo", "version": "0.1"}}, None
    if method == "tools/list":
        return {"tools": TOOLS}, None
    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        fn = HANDLERS.get(name)
        if fn is None:
            return None, {"code": -32601, "message": "unknown tool: %s" % name}
        try:
            text = fn(args)
            return {"content": [{"type": "text", "text": str(text)}], "isError": False}, None
        except Exception as e:  # 工具内部错误：包成 isError 结果，不打断连接
            return {"content": [{"type": "text", "text": "error: %s" % e}], "isError": True}, None
    return None, {"code": -32601, "message": "method not found: %s" % method}


# main: 逐行读取 stdin 上的 JSON-RPC 请求，处理后把响应逐行写回 stdout。
def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        rid = req.get("id")
        result, error = handle(req.get("method"), req.get("params") or {})
        resp = {"jsonrpc": "2.0", "id": rid}
        if error is not None:
            resp["error"] = error
        else:
            resp["result"] = result
        sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
