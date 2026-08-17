// mcp_client.hpp
// ---------------------------------------------------------------------------
// harness 侧的 MCP 客户端：把一个外部 MCP Server（独立进程）接进来。
//
// 它做三件事，正好对应 MCP 的核心语义：
//   1) start()    —— 以子进程方式拉起 server，建立 stdin/stdout 双向管道；
//   2) discover() —— 发 JSON-RPC "tools/list"，**运行时动态发现**对方暴露的工具；
//   3) call()     —— 发 JSON-RPC "tools/call" 执行某个远端工具，取回文本结果。
//
// 关键点：本文件对主程序几乎零侵入。它自带一套极小的 JSON 处理，不依赖主文件的
// 任何符号；主文件只需在「工具注册处」把 discover() 的结果并入本地工具，在「dispatch
// 兜底」把未知工具名转交 call()。harness 的循环/记忆/审批一律不改。
//
// 传输：JSON-RPC 2.0，逐行 JSON（每条消息一行、以 '\n' 结尾），POSIX 管道 + fork/exec。
// ---------------------------------------------------------------------------
#ifndef MCP_CLIENT_HPP
#define MCP_CLIENT_HPP

#include <string>
#include <vector>
#include <map>
#include <utility>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>

namespace mcp {

// McpToolInfo: 从 server 发现到的一个工具的元信息（自包含，不耦合主程序的 ToolSpec）。
// params: (参数名, 是否必填)。主程序会把它转成自己的 ToolSpec。
struct McpToolInfo {
    std::string name;
    std::string description;
    std::vector<std::pair<std::string, bool>> params;
};

// json_quote: 把字符串编码成合法 JSON 字符串字面量（含首尾引号）。本头文件自用版本。
inline std::string json_quote(const std::string &s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;
        }
    }
    return o + "\"";
}

// json_unescape: 还原 JSON 字符串字面量里的常见转义序列。
inline std::string json_unescape(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') { o += s[i]; continue; }
        if (++i >= s.size()) break;
        switch (s[i]) {
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case '"': o += '"';  break;
            case '\\': o += '\\'; break;
            case '/': o += '/';  break;
            default:  o += s[i];
        }
    }
    return o;
}

// find_string_value: 从 json 的 [from, ...) 段里找 "key" 对应的字符串值，返回 (值, 值结束位置)。
// 找不到时返回 ("", npos)。用于抽取 name/description/text 等标量字段。
inline std::pair<std::string, size_t> find_string_value(const std::string &json,
                                                        const std::string &key,
                                                        size_t from = 0) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return {"", std::string::npos};
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return {"", std::string::npos};
    size_t q = json.find('"', colon);
    if (q == std::string::npos) return {"", std::string::npos};
    std::string raw;
    size_t i = q + 1;
    for (; i < json.size(); ++i) {
        if (json[i] == '\\') { raw += json[i]; if (++i < json.size()) raw += json[i]; continue; }
        if (json[i] == '"') break;
        raw += json[i];
    }
    return {json_unescape(raw), i};
}

// McpClient: 管理一个 MCP server 子进程，并在其上收发 JSON-RPC。
class McpClient {
public:
    McpClient() = default;
    ~McpClient() { stop(); }

    // ok: 子进程是否已成功启动。
    bool ok() const { return to_child_ && from_child_; }

    // last_error: 最近一次失败的原因（供主程序打印诊断）。
    const std::string &last_error() const { return err_; }

    // start: 用给定命令启动 server 子进程，建立双向管道。argv 以可执行名开头。
    // 返回 true 表示进程已拉起（不代表握手成功，握手在 discover 里做）。
    bool start(const std::vector<std::string> &argv) {
        int in_pipe[2], out_pipe[2];   // in: 父->子(子的stdin)；out: 子->父(子的stdout)
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) { err_ = "pipe() failed"; return false; }
        pid_ = fork();
        if (pid_ < 0) { err_ = "fork() failed"; return false; }
        if (pid_ == 0) {
            // 子进程：把管道接到 stdin/stdout，再 exec 成 server。
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            std::vector<char *> cargv;
            for (auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
            cargv.push_back(nullptr);
            execvp(cargv[0], cargv.data());
            _exit(127);  // exec 失败
        }
        // 父进程：保留写入子 stdin 的一端、读取子 stdout 的一端。
        close(in_pipe[0]);
        close(out_pipe[1]);
        to_child_   = fdopen(in_pipe[1], "w");
        from_child_ = fdopen(out_pipe[0], "r");
        if (!to_child_ || !from_child_) { err_ = "fdopen() failed"; return false; }
        return true;
    }

    // stop: 关闭管道、回收子进程（析构时自动调用）。
    void stop() {
        if (to_child_)   { fclose(to_child_);   to_child_ = nullptr; }
        if (from_child_) { fclose(from_child_); from_child_ = nullptr; }
        if (pid_ > 0) { int st; waitpid(pid_, &st, 0); pid_ = -1; }
    }

    // discover: 握手并拉取工具清单。成功返回工具列表；失败返回空并置 last_error。
    std::vector<McpToolInfo> discover() {
        std::vector<McpToolInfo> tools;
        if (!ok()) { err_ = "client not started"; return tools; }
        // 1) initialize 握手（失败不致命，继续尝试 tools/list）。
        rpc("initialize", "{}");
        // 2) tools/list：动态发现。
        std::string resp = rpc("tools/list", "{}");
        if (resp.empty()) { err_ = "no response to tools/list"; return tools; }
        parse_tools_list(resp, tools);
        return tools;
    }

    // call: 调用远端工具。args 里的值统一按 JSON 字符串发送（够用于 path/text 类参数）。
    // 返回工具的文本结果；传输失败时返回以 "error:" 开头的说明。
    std::string call(const std::string &name, const std::map<std::string, std::string> &args) {
        if (!ok()) return "error: mcp client not started";
        std::string params = "{\"name\":" + json_quote(name) + ",\"arguments\":{";
        size_t i = 0;
        for (auto &kv : args) {
            params += json_quote(kv.first) + ":" + json_quote(kv.second);
            if (++i < args.size()) params += ",";
        }
        params += "}}";
        std::string resp = rpc("tools/call", params);
        if (resp.empty()) return "error: no response from mcp server";
        // 结果结构：{"result":{"content":[{"type":"text","text":"..."}], "isError":bool}}
        auto text = find_string_value(resp, "text");
        if (text.second == std::string::npos) {
            auto emsg = find_string_value(resp, "message");  // JSON-RPC error 对象
            return emsg.second == std::string::npos ? "error: unparseable mcp result"
                                                    : ("error: " + emsg.first);
        }
        return text.first;
    }

private:
    FILE *to_child_ = nullptr;
    FILE *from_child_ = nullptr;
    pid_t pid_ = -1;
    int next_id_ = 1;
    std::string err_;

    // rpc: 发一条 JSON-RPC 请求（method + 已拼好的 params JSON），阻塞读回一行响应。
    std::string rpc(const std::string &method, const std::string &params_json) {
        if (!ok()) return "";
        std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id_++) +
                           ",\"method\":" + json_quote(method) +
                           ",\"params\":" + params_json + "}\n";
        if (fputs(line.c_str(), to_child_) < 0) { err_ = "write to server failed"; return ""; }
        fflush(to_child_);
        return read_line();
    }

    // read_line: 从子进程 stdout 读取一整行（自动扩容，容纳较长的工具清单）。
    std::string read_line() {
        std::string out;
        char buf[4096];
        while (fgets(buf, sizeof(buf), from_child_)) {
            out += buf;
            if (!out.empty() && out.back() == '\n') break;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }

    // parse_tools_list: 从 tools/list 响应里切分出各 tool 对象，抽取 name/description/参数。
    // 用括号配对切分数组元素，再在每个对象里做浅字段抽取；够覆盖扁平 inputSchema。
    static void parse_tools_list(const std::string &resp, std::vector<McpToolInfo> &out) {
        size_t arr = resp.find("\"tools\"");
        if (arr == std::string::npos) return;
        size_t lb = resp.find('[', arr);
        if (lb == std::string::npos) return;
        size_t i = lb + 1, depth = 0, obj_start = std::string::npos;
        for (; i < resp.size(); ++i) {
            char c = resp[i];
            if (c == '{') { if (depth == 0) obj_start = i; ++depth; }
            else if (c == '}') {
                if (depth > 0 && --depth == 0 && obj_start != std::string::npos) {
                    out.push_back(parse_one_tool(resp.substr(obj_start, i - obj_start + 1)));
                    obj_start = std::string::npos;
                }
            } else if (c == ']' && depth == 0) {
                break;  // 工具数组结束
            }
        }
    }

    // parse_one_tool: 解析单个工具对象，抽取 name、description、properties 键名与 required 列表。
    static McpToolInfo parse_one_tool(const std::string &obj) {
        McpToolInfo t;
        t.name = find_string_value(obj, "name").first;
        t.description = find_string_value(obj, "description").first;

        // 收集 required 里的参数名（用于标记必填）。
        std::vector<std::string> required;
        size_t rq = obj.find("\"required\"");
        if (rq != std::string::npos) {
            size_t lb = obj.find('[', rq), rb = obj.find(']', rq);
            if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                std::string seg = obj.substr(lb + 1, rb - lb - 1);
                size_t p = 0;
                while ((p = seg.find('"', p)) != std::string::npos) {
                    size_t q = seg.find('"', p + 1);
                    if (q == std::string::npos) break;
                    required.push_back(seg.substr(p + 1, q - p - 1));
                    p = q + 1;
                }
            }
        }

        // 从 properties 对象里收集参数名。properties 是 {"x":{...},"y":{...}}，取顶层键。
        size_t pp = obj.find("\"properties\"");
        if (pp != std::string::npos) {
            size_t lb = obj.find('{', pp);
            if (lb != std::string::npos) {
                size_t j = lb + 1, depth = 0;
                bool expect_key = true;
                for (; j < obj.size(); ++j) {
                    char c = obj[j];
                    if (c == '{') ++depth;
                    else if (c == '}') { if (depth == 0) break; --depth; }
                    else if (c == '"' && depth == 0 && expect_key) {
                        size_t q = obj.find('"', j + 1);
                        if (q == std::string::npos) break;
                        std::string key = obj.substr(j + 1, q - j - 1);
                        bool req = false;
                        for (auto &r : required) if (r == key) { req = true; break; }
                        t.params.push_back({key, req});
                        j = q + 1;
                        expect_key = false;  // 下一个是该键的值对象，跳过其内部
                    } else if (c == ',' && depth == 0) {
                        expect_key = true;
                    }
                }
            }
        }
        return t;
    }
};

}  // namespace mcp

#endif  // MCP_CLIENT_HPP
