// mini_coding_agent.cpp
// ---------------------------------------------------------------------------
// 一个「最小 harness」的 C++ 教学实现，对照 rasbt/mini-coding-agent 的 Python 版本。
// 目标：用最少的代码把「agent = 在循环里反复(拼prompt -> 调模型 -> 解析 -> 执行工具 -> 记录)」
// 这件事显式地摊开。仅依赖 C++17 标准库，g++ -std=c++17 mini_coding_agent.cpp 即可编译。
//
// 六大组件 -> 代码符号 的映射（与 Python 版一一对应）：
//   1) Live Repo Context            -> WorkspaceContext
//   2) Prompt Shape & Cache Reuse   -> build_prefix / memory_text / prompt
//   3) Tools + Validation + Perms   -> tools_ / run_tool / approve / repeated_tool_call / tool_*
//   4) Context Reduction            -> clip / history_text
//   5) Transcript + Memory + Resume -> history_ / memory_ / record / note_tool / remember
//   6) Delegation & Bounded Subagent-> tool_delegate
//
// 模型端(ModelClient)是一个接口：这里给了一个脚本化的 FakeModelClient 让整个循环能离线跑；
// 想接真实模型，照着注释实现一个 OllamaModelClient 即可（发 HTTP 到 /api/generate）。
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ===========================================================================
// 通用小工具
// ===========================================================================

// clip: 把过长文本截断到 limit，并标注被截掉多少字符（对应组件4的输出裁剪）。
static std::string clip(const std::string &text, size_t limit = 4000) {
    if (text.size() <= limit) return text;
    return text.substr(0, limit) + "\n...[truncated " +
           std::to_string(text.size() - limit) + " chars]";
}

// trim: 去掉字符串首尾空白，解析模型输出时常用。
static std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// now: 返回一个粗略的时间戳字符串，仅用于给历史条目打标记。
static std::string now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

// sh_capture: 执行一条 shell 命令并抓取其标准输出（用于收集 git 信息 / run_shell 工具）。
static std::string sh_capture(const std::string &cmd) {
    std::string out;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

// ===========================================================================
// HTTP/JSON 辅助 —— 供远程模型客户端拼请求、解析响应、保护 shell 参数
// ===========================================================================

// json_quote: 把任意字符串编码成一个合法的 JSON 字符串字面量（含首尾双引号）。
// 转义引号、反斜杠、换行等控制字符，否则把多行 prompt 塞进 JSON body 会破坏结构。
static std::string json_quote(const std::string &s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    o += "\"";
    return o;
}

// shell_quote: 把字符串包成单引号安全的 shell 参数（用于把 JSON body 传给 curl -d）。
// 单引号内一切原样，遇到内部的单引号用 '\'' 收尾再拼回，避免命令注入与解析错乱。
static std::string shell_quote(const std::string &s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
}

// json_unescape: 把 JSON 字符串字面量里的转义序列还原成原始文本（extract 的配套）。
static std::string json_unescape(const std::string &s) {
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
            case 'u': {  // \uXXXX：只处理常见的 BMP 内码点，够解析模型输出
                if (i + 4 < s.size()) {
                    int code = std::stoi(s.substr(i + 1, 4), nullptr, 16);
                    if (code < 0x80) {
                        o += static_cast<char>(code);
                    } else if (code < 0x800) {
                        o += static_cast<char>(0xC0 | (code >> 6));
                        o += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        o += static_cast<char>(0xE0 | (code >> 12));
                        o += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        o += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    i += 4;
                }
                break;
            }
            default: o += s[i];
        }
    }
    return o;
}

// extract_json_string: 从一段 JSON 文本里抠出指定 key 对应的字符串值（浅解析，不引第三方库）。
// 做法：定位 "key"，跳到其后第一个双引号，逐字符扫描到未转义的收尾双引号，再做反转义。
static std::string extract_json_string(const std::string &json, const std::string &key) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t q = json.find('"', colon);
    if (q == std::string::npos) return "";
    size_t i = q + 1;
    std::string raw;
    for (; i < json.size(); ++i) {
        if (json[i] == '\\') { raw += json[i]; if (++i < json.size()) raw += json[i]; continue; }
        if (json[i] == '"') break;  // 未转义的双引号 = 字符串结束
        raw += json[i];
    }
    return json_unescape(raw);
}


// ===========================================================================
// 组件 1：Live Repo Context —— 开局收集一次的稳定环境事实
// ===========================================================================
//这个结构体是工作区快照，包含了当前工作目录、项目根目录、当前分支、当前状态、项目文档片段
struct WorkspaceContext {
    std::string cwd;//当前工作目录
    std::string repo_root;//项目根目录
    std::string branch;//当前分支
    std::string status;//当前状态
    std::string docs;  // 项目文档片段(README 等)

    // build: 启动时跑几条 git 命令 + 读取项目文档，拼出一份静态工作区快照。
    static WorkspaceContext build(const std::string &dir) {
        WorkspaceContext w;
        w.cwd = fs::weakly_canonical(dir).string();//获取当前工作目录的规范路径
        std::string root = trim(sh_capture("git -C \"" + w.cwd + "\" rev-parse --show-toplevel 2>/dev/null"));
        w.repo_root = root.empty() ? w.cwd : root;
        w.branch = trim(sh_capture("git -C \"" + w.repo_root + "\" branch --show-current 2>/dev/null"));
        if (w.branch.empty()) w.branch = "-";
        std::string st = trim(sh_capture("git -C \"" + w.repo_root + "\" status --short 2>/dev/null"));
        w.status = st.empty() ? "clean" : clip(st, 1500);

        // 只读少量约定文档名，控制注入 prompt 的体积。
        const char *names[] = {"AGENTS.md", "README.md", "pyproject.toml"};
        std::string acc;
        for (const char *n : names) {
            fs::path p = fs::path(w.repo_root) / n;
            if (!fs::exists(p)) continue;
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            acc += std::string("- ") + n + "\n" + clip(ss.str(), 800) + "\n";
        }
        w.docs = acc.empty() ? "- none" : acc;
        return w;
    }

    // text: 把工作区快照序列化成一段文本，供 prompt 前缀使用。
    std::string text() const {
        std::ostringstream o;
        o << "Workspace:\n"
          << "- cwd: " << cwd << "\n"
          << "- repo_root: " << repo_root << "\n"
          << "- branch: " << branch << "\n"
          << "- status:\n" << status << "\n"
          << "- project_docs:\n" << docs;
        return o.str();
    }
};

// ===========================================================================
// 模型客户端接口 —— harness 与「大脑」之间唯一的边界
// ===========================================================================
struct ModelClient {
    virtual ~ModelClient() = default;
    // complete: 输入一段完整 prompt 文本，返回模型生成的文本。模型是无状态的。
    virtual std::string complete(const std::string &prompt, int max_new_tokens) = 0;
};

// FakeModelClient: 用预设脚本按顺序吐输出，让整条 agent 循环无需真实模型也能跑通。
// （接真实模型时，改写成 OllamaModelClient：把 prompt POST 到 /api/generate 并取 response 字段。）
struct FakeModelClient : ModelClient {
    std::deque<std::string> outputs;
    std::vector<std::string> seen_prompts;  // 记录每轮实际喂进去的 prompt，便于观察

    explicit FakeModelClient(std::vector<std::string> outs)
        : outputs(outs.begin(), outs.end()) {}

    // complete: 记录本轮 prompt，然后弹出脚本里的下一条输出。
    std::string complete(const std::string &prompt, int) override {
        seen_prompts.push_back(prompt);
        if (outputs.empty()) return "<final>fake model ran out of outputs</final>";
        std::string out = outputs.front();
        outputs.pop_front();
        return out;
    }
};

// RemoteModelClient: 调用「OpenAI 兼容」的远程 Chat Completions API（/v1/chat/completions）。
// 只要服务遵循该事实标准（OpenAI / DeepSeek / OpenRouter / Moonshot 等），换个 base_url + model 即可。
// 与 FakeModelClient 的唯一区别：complete() 不查脚本，而是带着 API key 发 HTTPS 请求、等模型真实推理。
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

    // complete: 把 harness 拼好的 prompt 作为一条 user 消息发给远程模型，取回 assistant 文本。
    std::string complete(const std::string &prompt, int max_new_tokens) override {
        // 1) 拼 Chat Completions 请求体：单条 user 消息即可，harness 已把全部上下文塞进 prompt。
        std::string body = std::string("{")
            + "\"model\":" + json_quote(model) + ","
            + "\"temperature\":" + std::to_string(temperature) + ","
            + "\"max_tokens\":" + std::to_string(max_new_tokens) + ","
            + "\"messages\":[{\"role\":\"user\",\"content\":" + json_quote(prompt) + "}]"
            + "}";

        // 2) 发 HTTPS POST，带上 Authorization: Bearer <key>。演示用 curl 子进程；生产建议 libcurl。
        //    -s 静默、--fail-with-body 让 4xx/5xx 也把响应体带回来便于排错。
        std::string url = base_url + "/chat/completions";
        std::string cmd = "curl -s --fail-with-body -X POST " + shell_quote(url)
            + " -H 'Content-Type: application/json'"
            + " -H " + shell_quote("Authorization: Bearer " + api_key)
            + " -d " + shell_quote(body);
        std::string raw = sh_capture(cmd);

        // 3) 从响应里抠出 choices[0].message.content。浅解析：先定位 content 字段即可。
        std::string content = extract_json_string(raw, "content");
        if (content.empty()) {
            // 取不到内容通常是鉴权失败/限流/模型名错误：把原始响应回传给 harness，
            // 让它作为一次坏输出走 parse 的 Retry 分支，而不是静默失败。
            std::string err = extract_json_string(raw, "message");
            return "<final>remote model error: " + (err.empty() ? clip(raw, 300) : err) + "</final>";
        }
        return content;
    }
};

// ===========================================================================
// 数据结构：历史条目 / 蒸馏记忆 / 工具元数据 / 解析结果
// ===========================================================================

// HistoryItem: 一条完整流水账（用户输入、模型回答、或一次工具调用及其结果）。
struct HistoryItem {
    std::string role;                          // "user" / "assistant" / "tool"
    std::string name;                          // 当 role=="tool" 时的工具名
    std::map<std::string, std::string> args;   // 工具参数
    std::string content;                       // 文本内容 / 工具返回
    std::string created_at;
};

// Memory: 蒸馏后的工作记忆，只保留「当前任务 + 碰过的文件 + 最近几条笔记」。
struct Memory {
    std::string task;
    std::vector<std::string> files;
    std::vector<std::string> notes;
};

// ToolSpec: 一个工具的元数据（名字、参数模式、是否危险、描述），用于生成 prompt 里的工具清单。
struct ToolSpec {
    std::string name;
    std::vector<std::pair<std::string, std::string>> schema;  // 保留声明顺序
    bool risky;
    std::string description;
};

// ParseKind: 模型每轮输出被归类为三种之一。
enum class ParseKind { Tool, Final, Retry };

// ParseResult: parse() 的结构化产物。
struct ParseResult {
    ParseKind kind;//解析结果类型
    std::string name;                          // Tool: 工具名
    std::map<std::string, std::string> args;   // Tool: 参数
    std::string text;                          // Final: 最终答案 / Retry: 提示语
};

// ===========================================================================
// MiniAgent —— harness 本体
// ===========================================================================
class MiniAgent {
public:
    // MiniAgent: 构造一个 agent，绑定模型、工作区、审批策略与各类预算上限。
    //入参是模型客户端，工作区快照，审批策略，最大工具调用次数，最大递归深度，是否只读模式
    MiniAgent(ModelClient &model, const WorkspaceContext &ws,
              std::string approval = "ask", int max_steps = 6,
              int depth = 0, int max_depth = 1, bool read_only = false)
        : model_(model), ws_(ws), root_(ws.repo_root), approval_(approval),
          max_steps_(max_steps), depth_(depth), max_depth_(max_depth),
          read_only_(read_only) {
        build_tools();
        prefix_ = build_prefix();  // 前缀只构建一次，便于 KV-cache 复用（组件2的核心）
    }

    // -----------------------------------------------------------------------
    // 心脏：ask() —— 整个 agent 就是这个 while 循环
    // -----------------------------------------------------------------------
    // ask: 接收一条用户请求，反复(拼prompt->调模型->解析->执行工具->记录)直到给出最终答案或触顶。
    std::string ask(const std::string &user_message) {
        if (memory_.task.empty()) memory_.task = clip(trim(user_message), 300);
        record({"user", "", {}, user_message, now()});

        int tool_steps = 0;
        int attempts = 0;
        const int max_attempts = std::max(max_steps_ * 3, max_steps_ + 4);
        // 主循环：反复拼prompt->调模型->解析->执行工具->记录，直到触顶。
        //tool_steps: 工具调用次数，attempts: 模型调用次数
        while (tool_steps < max_steps_ && attempts < max_attempts) {
            ++attempts;
            std::string raw = model_.complete(prompt(user_message), 512);
            ParseResult r = parse(raw);//解析模型输出，得到解析结果

            if (r.kind == ParseKind::Tool) {
                ++tool_steps;  // 只有真正的工具调用才消耗任务预算
                std::string result = run_tool(r.name, r.args);//执行工具，得到结果
                record({"tool", r.name, r.args, result, now()});//记录工具调用
                note_tool(r.name, r.args, result);//记录工具调用结果    
                continue;
            }
            if (r.kind == ParseKind::Retry) {
                // 模型格式坏了：记录一下但不烧 step 预算，直接重来。
                record({"assistant", "", {}, r.text, now()});
                continue;
            }
            // Final：给出最终答案，结束。
            record({"assistant", "", {}, r.text, now()});
            remember(memory_.notes, clip(r.text, 220), 5);
            return r.text;
        }
        std::string final = "Stopped after reaching the step limit without a final answer.";
        record({"assistant", "", {}, final, now()});
        return final;
    }

    // memory_text: 暴露蒸馏记忆的文本，供 /memory 之类的命令查看。
    std::string memory_text() const {
        std::ostringstream o;
        o << "Memory:\n- task: " << (memory_.task.empty() ? "-" : memory_.task) << "\n- files: ";
        for (size_t i = 0; i < memory_.files.size(); ++i)
            o << memory_.files[i] << (i + 1 < memory_.files.size() ? ", " : "");
        o << "\n- notes:\n";
        for (auto &n : memory_.notes) o << "- " << n << "\n";
        return o.str();
    }

private:
    ModelClient &model_;//模型客户端
    WorkspaceContext ws_;//工作区快照
    fs::path root_;//项目根目录
    std::string approval_;//审批策略
    int max_steps_;//最大工具调用次数
    int depth_;//当前递归深度
    int max_depth_;//最大递归深度
    bool read_only_;//是否只读模式
    //数组存储工具的元数据（名字、参数模式、是否危险、描述）
    std::vector<ToolSpec> tools_;      // 工具元数据（用于展示 + 危险判定）
    std::string prefix_;               // 静态 prompt 前缀
    std::vector<HistoryItem> history_; // 完整流水账（组件5）
    Memory memory_;                    // 蒸馏记忆（组件5）

    // =======================================================================
    // 组件 3：Structured Tools —— 声明工具元数据
    // =======================================================================
    // build_tools: 登记全部工具的元数据；depth 未超限时才追加危险的 delegate 工具（组件6）。
    void build_tools() {
        tools_ = {
            {"list_files", {{"path", "str='.'"}}, false, "List files in the workspace."},
            {"read_file", {{"path", "str"}, {"start", "int=1"}, {"end", "int=200"}}, false, "Read a file by line range."},
            {"search", {{"pattern", "str"}, {"path", "str='.'"}}, false, "Search the workspace for a substring."},
            {"run_shell", {{"command", "str"}}, true, "Run a shell command in the repo root."},
            {"write_file", {{"path", "str"}, {"content", "str"}}, true, "Write a text file."},
            {"patch_file", {{"path", "str"}, {"old_text", "str"}, {"new_text", "str"}}, true, "Replace one exact text block in a file."},
        };
        if (depth_ < max_depth_)
            tools_.push_back({"delegate", {{"task", "str"}}, false, "Ask a bounded read-only child agent to investigate."});
    }

    // is_risky: 查询某工具是否为危险工具（需要审批）。
    bool is_risky(const std::string &name) const {
        for (auto &t : tools_) if (t.name == name) return t.risky;
        return false;
    }

    // has_tool: 判断工具名是否已登记。
    bool has_tool(const std::string &name) const {
        for (auto &t : tools_) if (t.name == name) return true;
        return false;
    }

    // =======================================================================
    // 组件 2：Prompt Shape —— 静态前缀 + 动态部分
    // =======================================================================
    // build_prefix: 拼出「不变的」prompt 前缀（角色 + 规则 + 工具清单 + 示例 + 工作区快照）。
    // 只在构造时调用一次，保证多轮请求共享同一前缀，利于模型侧 KV-cache 复用。
    std::string build_prefix() const {
        std::ostringstream tools;
        for (auto &t : tools_) {
            tools << "- " << t.name << "(";
            for (size_t i = 0; i < t.schema.size(); ++i)
                tools << t.schema[i].first << ": " << t.schema[i].second
                      << (i + 1 < t.schema.size() ? ", " : "");
            tools << ") [" << (t.risky ? "approval required" : "safe") << "] " << t.description << "\n";
        }
        std::ostringstream o;
        o << "You are Mini-Coding-Agent, a small local coding agent.\n\n"
          << "Rules:\n"
          << "- Use tools instead of guessing about the workspace.\n"
          << "- Return exactly one <tool .../> call or one <final>...</final> answer.\n"
          << "- Tool calls look like: <tool name=\"read_file\" path=\"README.md\" start=\"1\" end=\"20\"></tool>\n"
          << "- For file bodies use: <tool name=\"write_file\" path=\"x.txt\"><content>...</content></tool>\n"
          << "- Never invent tool results. Keep answers concise.\n\n"
          << "Tools:\n" << tools.str() << "\n"//把所有的工具放进来
          << ws_.text();//把工作区快照也放进来
        return o.str();
    }

    // prompt: 每轮把「静态前缀 + 蒸馏记忆 + 历史 + 当前请求」拼成最终喂给模型的完整文本。
    std::string prompt(const std::string &user_message) const {
        std::ostringstream o;
        o << prefix_ << "\n\n" << memory_text() << "\nTranscript:\n"
          << history_text() << "\n\nCurrent user request:\n" << user_message;
        return o.str();
    }

    // =======================================================================
    // 组件 4：Context Reduction —— 历史瘦身
    // =======================================================================
    // history_text: 把历史序列化成文本，并做三件事：分级裁剪、重复读取去重、写操作使旧读取失效。
    std::string history_text() const {
        if (history_.empty()) return "- empty";
        std::vector<std::string> lines;
        std::vector<std::string> seen_reads;  // 已经读过的文件路径
        size_t recent_start = history_.size() > 6 ? history_.size() - 6 : 0;

        for (size_t i = 0; i < history_.size(); ++i) {
            const auto &it = history_[i];
            bool recent = i >= recent_start;

            // 写操作后，把该文件从「已读集合」移除：文件已变，旧读取内容作废。
            if (it.role == "tool" && (it.name == "write_file" || it.name == "patch_file")) {
                auto p = it.args.count("path") ? it.args.at("path") : "";
                seen_reads.erase(std::remove(seen_reads.begin(), seen_reads.end(), p), seen_reads.end());
            }
            // 较早的重复 read_file 直接跳过（去重）。
            if (it.role == "tool" && it.name == "read_file" && !recent) {
                auto p = it.args.count("path") ? it.args.at("path") : "";
                if (std::find(seen_reads.begin(), seen_reads.end(), p) != seen_reads.end()) continue;
                seen_reads.push_back(p);
            }

            size_t limit = recent ? 900 : 180;  // 越近的历史给越大额度
            if (it.role == "tool") {
                lines.push_back("[tool:" + it.name + "] " + args_to_string(it.args));
                lines.push_back(clip(it.content, limit));
            } else {
                lines.push_back("[" + it.role + "] " + clip(it.content, limit));
            }
        }
        std::string joined;
        for (auto &l : lines) joined += l + "\n";
        return clip(joined, 12000);
    }

    // args_to_string: 把参数 map 序列化成稳定的字符串（便于日志与去重比较）。
    static std::string args_to_string(const std::map<std::string, std::string> &args) {
        std::ostringstream o;
        o << "{";
        size_t i = 0;
        for (auto &kv : args) {
            o << "\"" << kv.first << "\":\"" << clip(kv.second, 80) << "\"";
            if (++i < args.size()) o << ",";
        }
        o << "}";
        return o.str();
    }

    // =======================================================================
    // 组件 5：Transcript + Memory —— 两级记忆
    // =======================================================================
    // record: 把一条历史追加到完整流水账（真实项目里此处会立即落盘以支持续接）。
    void record(const HistoryItem &item) { history_.push_back(item); }

    // note_tool: 从一次工具调用里蒸馏出「碰过的文件」与「一句话笔记」写入工作记忆。
    void note_tool(const std::string &name, const std::map<std::string, std::string> &args,
                   const std::string &result) {
        if ((name == "read_file" || name == "write_file" || name == "patch_file") && args.count("path"))
            remember(memory_.files, args.at("path"), 8);
        std::string note = name + ": " + clip(result, 200);
        std::replace(note.begin(), note.end(), '\n', ' ');
        remember(memory_.notes, note, 5);
    }

    // remember: 「最近使用优先 + 固定容量」的滑动窗口：先去重、再追加到末尾、再截断到 limit。
    static void remember(std::vector<std::string> &bucket, const std::string &item, size_t limit) {
        if (item.empty()) return;
        bucket.erase(std::remove(bucket.begin(), bucket.end(), item), bucket.end());
        bucket.push_back(item);
        if (bucket.size() > limit) bucket.erase(bucket.begin(), bucket.end() - limit);
    }

    // =======================================================================
    // 组件 3：run_tool —— 执行前的多道闸门
    // =======================================================================
    // run_tool: 工具执行总入口，依次过「存在性 -> 防重复 -> 权限审批 -> 执行并裁剪输出」四道闸门。
    std::string run_tool(const std::string &name, std::map<std::string, std::string> args) {
        if (!has_tool(name)) return "error: unknown tool '" + name + "'";//未知工具
        if (repeated_tool_call(name, args))
            return "error: repeated identical tool call for " + name + "; choose a different tool";
        if (is_risky(name) && !approve(name, args))
            return "error: approval denied for " + name;
        try {
            return clip(dispatch(name, args));
        } catch (const std::exception &e) {
            return std::string("error: tool ") + name + " failed: " + e.what();
        }
    }

    // repeated_tool_call: 若最近两次工具调用与本次完全相同，则判定为「鬼打墙」，拦截。
    bool repeated_tool_call(const std::string &name, const std::map<std::string, std::string> &args) const {
        std::vector<const HistoryItem *> tools;
        for (auto &it : history_) if (it.role == "tool") tools.push_back(&it);
        if (tools.size() < 2) return false;
        for (size_t k = tools.size() - 2; k < tools.size(); ++k)
            if (!(tools[k]->name == name && tools[k]->args == args)) return false;
        return true;
    }

    // approve: 危险工具的权限闸门。read_only 一律拒绝；auto 放行；never 拒绝；ask 走交互式确认。
    bool approve(const std::string &name, const std::map<std::string, std::string> &args) {
        if (read_only_) return false;
        if (approval_ == "auto") return true;
        if (approval_ == "never") return false;
        std::cout << "approve " << name << " " << args_to_string(args) << "? [y/N] " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        line = trim(line);
        return line == "y" || line == "Y" || line == "yes";
    }

    // dispatch: 按工具名派发到具体实现。参数校验就地进行，失败则抛异常由 run_tool 兜住。
    std::string dispatch(const std::string &name, std::map<std::string, std::string> &args) {
        if (name == "list_files") return tool_list_files(args);
        if (name == "read_file") return tool_read_file(args);
        if (name == "search") return tool_search(args);
        if (name == "run_shell") return tool_run_shell(args);
        if (name == "write_file") return tool_write_file(args);
        if (name == "patch_file") return tool_patch_file(args);
        if (name == "delegate") return tool_delegate(args);
        throw std::runtime_error("no dispatch");
    }

    // =======================================================================
    // 解析：把模型的文本输出解读为「工具调用 / 最终答案 / 需重试」
    // =======================================================================
    // parse: 判断输出里 <tool> 与 <final> 谁先出现，据此归类；空或畸形则要求重试。
    ParseResult parse(const std::string &raw) const {
        size_t pos_tool = raw.find("<tool");
        size_t pos_final = raw.find("<final>");
        bool tool_first = pos_tool != std::string::npos &&
                          (pos_final == std::string::npos || pos_tool < pos_final);
        if (tool_first) {
            ParseResult r = parse_tool(raw);
            if (r.name.empty())
                return {ParseKind::Retry, "", {}, "malformed tool call; reply with a valid <tool> or <final>."};
            return r;
        }
        if (pos_final != std::string::npos) {
            std::string f = trim(extract(raw, "final"));
            if (f.empty()) return {ParseKind::Retry, "", {}, "empty <final> answer; try again."};
            return {ParseKind::Final, "", {}, f};
        }
        std::string t = trim(raw);
        if (!t.empty()) return {ParseKind::Final, "", {}, t};
        return {ParseKind::Retry, "", {}, "empty response; reply with <tool> or <final>."};
    }

    // parse_tool: 从 <tool ...>...</tool> 里抽出属性(name/path/...)与子标签(content/old_text/...)组成参数。
    ParseResult parse_tool(const std::string &raw) const {
        std::smatch m;
        std::regex block(R"(<tool([^>]*)>([\s\S]*?)</tool>)");
        if (!std::regex_search(raw, m, block)) return {ParseKind::Retry, "", {}, ""};
        std::string attrs = m[1].str();
        std::string body = m[2].str();

        std::map<std::string, std::string> args;
        std::regex attr(R"RX((\w+)\s*=\s*"([^"]*)")RX");
        for (auto it = std::sregex_iterator(attrs.begin(), attrs.end(), attr);
             it != std::sregex_iterator(); ++it)
            args[(*it)[1].str()] = (*it)[2].str();

        std::string name = args.count("name") ? args["name"] : "";
        args.erase("name");
        // 多行内容走子标签，避免让模型在属性里塞换行。
        for (const char *k : {"content", "old_text", "new_text", "command", "task", "pattern"}) {
            std::string open = std::string("<") + k + ">", close = std::string("</") + k + ">";
            size_t s = body.find(open);
            if (s == std::string::npos) continue;
            s += open.size();
            size_t e = body.find(close, s);
            args[k] = (e == std::string::npos) ? body.substr(s) : body.substr(s, e - s);
        }
        return {ParseKind::Tool, name, args, ""};
    }

    // extract: 取出 <tag>...</tag> 之间的内容（用于 <final>）。
    static std::string extract(const std::string &text, const std::string &tag) {
        std::string open = "<" + tag + ">", close = "</" + tag + ">";
        size_t s = text.find(open);
        if (s == std::string::npos) return text;
        s += open.size();
        size_t e = text.find(close, s);
        return e == std::string::npos ? text.substr(s) : text.substr(s, e - s);
    }

    // =======================================================================
    // 路径沙箱：所有文件操作必须落在仓库根目录内
    // =======================================================================
    // resolve: 把工具传来的相对/绝对路径规范化，并校验其不逃出 root_，否则抛异常。
    fs::path resolve(const std::string &raw) const {
        fs::path p(raw);
        if (!p.is_absolute()) p = root_ / p;
        fs::path rp = fs::weakly_canonical(p);
        fs::path rr = fs::weakly_canonical(root_);
        auto rs = rp.string(), rrs = rr.string();
        if (rs.compare(0, rrs.size(), rrs) != 0)
            throw std::runtime_error("path escapes workspace: " + raw);
        return rp;
    }

    // get_int: 从参数 map 里安全读取一个整数，缺省时用默认值。
    static int get_int(const std::map<std::string, std::string> &args, const std::string &key, int def) {
        auto it = args.find(key);
        if (it == args.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }

    // =======================================================================
    // 工具实现（组件3的具体动作）
    // =======================================================================
    // tool_list_files: 列出目录下的文件与子目录（跳过 .git 等噪声）。
    std::string tool_list_files(std::map<std::string, std::string> &args) {
        fs::path dir = resolve(args.count("path") ? args["path"] : ".");
        if (!fs::is_directory(dir)) throw std::runtime_error("path is not a directory");
        std::vector<std::string> lines;
        for (auto &entry : fs::directory_iterator(dir)) {
            std::string nm = entry.path().filename().string();
            if (nm == ".git" || nm == "__pycache__" || nm == ".venv") continue;
            std::string kind = entry.is_directory() ? "[D] " : "[F] ";
            lines.push_back(kind + fs::relative(entry.path(), root_).string());
        }
        std::sort(lines.begin(), lines.end());
        std::string out;
        for (auto &l : lines) out += l + "\n";
        return out.empty() ? "(empty)" : out;
    }

    // tool_read_file: 按行区间 [start,end] 读取文件，并给每行加上行号。
    std::string tool_read_file(std::map<std::string, std::string> &args) {
        fs::path p = resolve(args.at("path"));
        if (!fs::is_regular_file(p)) throw std::runtime_error("path is not a file");
        int start = get_int(args, "start", 1), end = get_int(args, "end", 200);
        if (start < 1 || end < start) throw std::runtime_error("invalid line range");
        std::ifstream f(p);
        std::string line;
        std::ostringstream o;
        o << "# " << fs::relative(p, root_).string() << "\n";
        for (int n = 1; std::getline(f, line); ++n) {
            if (n < start) continue;
            if (n > end) break;
            o << (n < 1000 ? " " : "") << n << ": " << line << "\n";
        }
        return o.str();
    }

    // tool_search: 在工作区里做简单的子串搜索，返回命中的「文件:行号:内容」。
    std::string tool_search(std::map<std::string, std::string> &args) {
        std::string pattern = trim(args.count("pattern") ? args["pattern"] : "");
        if (pattern.empty()) throw std::runtime_error("pattern must not be empty");
        fs::path base = resolve(args.count("path") ? args["path"] : ".");
        std::string out;
        int hits = 0;
        for (auto &entry : fs::recursive_directory_iterator(base)) {
            if (!entry.is_regular_file()) continue;
            std::string path = entry.path().string();
            if (path.find("/.git/") != std::string::npos) continue;
            std::ifstream f(entry.path());
            std::string line;
            for (int n = 1; std::getline(f, line); ++n) {
                if (line.find(pattern) != std::string::npos) {
                    out += fs::relative(entry.path(), root_).string() + ":" + std::to_string(n) + ":" + line + "\n";
                    if (++hits >= 100) return out;
                }
            }
        }
        return out.empty() ? "(no matches)" : out;
    }

    // tool_run_shell: 在仓库根目录执行一条 shell 命令，返回退出码与合并后的输出。
    std::string tool_run_shell(std::map<std::string, std::string> &args) {
        std::string cmd = trim(args.count("command") ? args["command"] : "");
        if (cmd.empty()) throw std::runtime_error("command must not be empty");
        std::string full = "cd \"" + root_.string() + "\" && " + cmd + " 2>&1";
        std::string out = sh_capture(full);
        return "output:\n" + (out.empty() ? "(empty)" : out);
    }

    // tool_write_file: 写入（或覆盖）一个文本文件，必要时创建父目录。
    std::string tool_write_file(std::map<std::string, std::string> &args) {
        fs::path p = resolve(args.at("path"));
        if (fs::is_directory(p)) throw std::runtime_error("path is a directory");
        if (!args.count("content")) throw std::runtime_error("missing content");
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << args["content"];
        return "wrote " + fs::relative(p, root_).string() + " (" +
               std::to_string(args["content"].size()) + " chars)";
    }

    // tool_patch_file: 精确替换文件中「恰好出现一次」的文本块，否则拒绝，保证改动可预期。
    std::string tool_patch_file(std::map<std::string, std::string> &args) {
        fs::path p = resolve(args.at("path"));
        if (!fs::is_regular_file(p)) throw std::runtime_error("path is not a file");
        std::string old_text = args.count("old_text") ? args["old_text"] : "";
        if (old_text.empty()) throw std::runtime_error("old_text must not be empty");
        if (!args.count("new_text")) throw std::runtime_error("missing new_text");
        std::ifstream in(p);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        // 统计出现次数，必须恰好一次。
        size_t count = 0, pos = 0;
        while ((pos = text.find(old_text, pos)) != std::string::npos) { ++count; pos += old_text.size(); }
        if (count != 1) throw std::runtime_error("old_text must occur exactly once, found " + std::to_string(count));
        text.replace(text.find(old_text), old_text.size(), args["new_text"]);
        std::ofstream out(p);
        out << text;
        return "patched " + fs::relative(p, root_).string();
    }

    // =======================================================================
    // 组件 6：Delegation —— 受约束的子 agent
    // =======================================================================
    // tool_delegate: 派生一个「只读、不能审批、深度+1」的子 agent 去调查子任务，天然禁止无限递归。
    std::string tool_delegate(std::map<std::string, std::string> &args) {
        if (depth_ >= max_depth_) throw std::runtime_error("delegate depth exceeded");
        std::string task = trim(args.count("task") ? args["task"] : "");
        if (task.empty()) throw std::runtime_error("task must not be empty");
        MiniAgent child(model_, ws_, /*approval=*/"never", /*max_steps=*/3,
                        /*depth=*/depth_ + 1, /*max_depth=*/max_depth_, /*read_only=*/true);
        child.memory_.task = task;
        child.memory_.notes = {clip(history_text(), 300)};  // 继承父 agent 的历史摘要作为背景
        return "delegate_result:\n" + child.ask(task);
    }
};

// ===========================================================================
// main —— 组装 workspace + 远程模型，让 agent 端到端跑一次任务
// ===========================================================================

// env_or: 读取环境变量，未设置时返回默认值（集中管理配置，避免把 key 硬编码进源码）。
static std::string env_or(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// main: 从环境变量读取远程模型配置（base_url/model/api_key），构造 agent 并执行一次任务。
int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    //建立工作区快照，记录当前目录下的所有文件，工作区快照
    WorkspaceContext ws = WorkspaceContext::build(dir);

    // 配置来自环境变量：
    //   AGENT_API_KEY   必填，远程模型的密钥（不写进源码/命令行，避免泄露）
    //   AGENT_BASE_URL  可选，OpenAI 兼容 API 根地址，默认 OpenAI 官方
    //   AGENT_MODEL     可选，模型名，默认 gpt-4o-mini
    std::string api_key  = env_or("AGENT_API_KEY", "");
    std::string base_url = env_or("AGENT_BASE_URL", "https://api.openai.com/v1");
    std::string model_name = env_or("AGENT_MODEL", "gpt-4o-mini");

    if (api_key.empty()) {
        std::cerr << "error: 环境变量 AGENT_API_KEY 未设置。\n"
                  << "用法示例:\n"
                  << "  export AGENT_API_KEY=sk-xxx\n"
                  << "  export AGENT_BASE_URL=https://api.openai.com/v1   # 可选\n"
                  << "  export AGENT_MODEL=gpt-4o-mini                    # 可选\n"
                  << "  ./mini_agent [workspace_dir]\n";
        return 1;
    }

    // 真实远程模型：换脑不换 harness——下面 MiniAgent 及其循环一个字都不用改。
    RemoteModelClient model(base_url, model_name, api_key);

    // approval=ask：真实模型可能提出危险操作，默认逐个确认（trusted 场景可改 auto）。
    MiniAgent agent(model, ws, /*approval=*/"ask");

    std::cout << "=== MINI CODING AGENT (C++) ===\n"
              << "workspace: " << ws.cwd << " | branch: " << ws.branch << "\n"
              << "model: " << model.model << " @ " << base_url << "\n\n";

    std::string answer = agent.ask("列出目录，读一下 README 开头，然后创建一个示例文件。");
    std::cout << "\n[final answer] " << answer << "\n\n";
    std::cout << agent.memory_text() << "\n";
    return 0;
}
