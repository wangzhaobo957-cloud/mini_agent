// mini_agent.hpp
// ---------------------------------------------------------------------------
// MiniAgent —— harness 本体。整个 agent 就是 ask() 里那个 while 循环：
// 反复(组装messages -> 调模型 -> 解析 -> 执行工具 -> 记录)，直到给出最终答案或触顶。
//
// 组件 -> 本文件符号 的映射：
//   2) Prompt Shape & Cache Reuse   -> build_prefix / memory_text / request_messages
//   3) Tools + Validation + Perms   -> build_tools / run_tool / approve / repeated_tool_call / tool_*
//   4) Context Reduction            -> clip / history_text
//   5) Transcript + Memory + Resume -> history_ / memory_ / record / note_tool / remember
//   6) Delegation & Bounded Subagent-> tool_delegate
// ---------------------------------------------------------------------------
#ifndef MINI_AGENT_HPP
#define MINI_AGENT_HPP

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agent_types.hpp"        // ToolSpec / ModelReply / HistoryItem / Memory
#include "mcp_client.hpp"         // MCP：把外部 server（独立进程）暴露的工具动态接入
#include "model_client.hpp"       // ModelClient
#include "text_utils.hpp"         // clip / trim / now / sh_capture
#include "workspace_context.hpp"  // WorkspaceContext / fs

// ===========================================================================
// MiniAgent —— harness 本体
// ===========================================================================
class MiniAgent {
public:
    // MiniAgent: 构造一个 agent，绑定模型、工作区、审批策略与各类预算上限。
    //入参是模型客户端，工作区快照，审批策略，最大工具调用次数，最大递归深度，是否只读模式
    //mcp: 可选的 MCP 客户端；非空且已连接时，其 server 暴露的工具会被动态接入本地工具集。
    MiniAgent(ModelClient &model, const WorkspaceContext &ws,
              std::string approval = "ask", int max_steps = 6,
              int depth = 0, int max_depth = 1, bool read_only = false,
              mcp::McpClient *mcp = nullptr)
        : model_(model), ws_(ws), root_(ws.repo_root), approval_(approval),
          max_steps_(max_steps), depth_(depth), max_depth_(max_depth),
          read_only_(read_only), mcp_(mcp) {
        build_tools();
        prefix_ = build_prefix();  // 前缀只构建一次，便于 KV-cache 复用（组件2的核心）
    }

    // -----------------------------------------------------------------------
    // 心脏：ask() —— 整个 agent 就是这个 while 循环
    // -----------------------------------------------------------------------
    // ask: 接收一条用户请求，反复(组装messages->调模型->执行工具->记录)直到给出最终答案或触顶。
    // function calling 下模型直接返回结构化回复，无需 parse/容错/Retry 分支。
    std::string ask(const std::string &user_message) {
        // 1) 记录用户输入
        if (memory_.task.empty()) memory_.task = clip(trim(user_message), 300);
        record({"user", "", {}, user_message, "", now(), {}});

        int tool_steps = 0;// 工具调用次数
        // 主循环：反复组装messages->调模型->执行工具->记录，直到给出最终答案或触顶。
        while (tool_steps < max_steps_) {
            // 把可用工具作为 tools 定义随请求一起发给模型（function calling 的声明端）。
            ModelReply r = model_.complete(request_messages(), tools_, 512);

            if (r.is_tool_call) {
                std::vector<ToolCall> calls = r.tool_calls;
                record({"assistant", "", {}, "", "", now(), calls});
                for (const ToolCall &call : calls) {
                    ++tool_steps;  // 每个具体工具调用都消耗任务预算
                    std::string result = run_tool(call.name, call.args);//执行工具，得到结果
                    record({"tool", call.name, call.args, result, call.id, now(), {}});//记录工具调用
                    note_tool(call.name, call.args, result);//记录工具调用结果
                }
                continue;
            }
            // 非工具调用：模型给出最终答案，结束。
            record({"assistant", "", {}, r.content, "", now(), {}});
            remember(memory_.notes, clip(r.content, 220), 5);
            return r.content;
        }
        std::string final = synthesize_after_step_limit();
        record({"assistant", "", {}, final, "", now(), {}});
        remember(memory_.notes, clip(final, 220), 5);
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
    mcp::McpClient *mcp_ = nullptr;    // 可选的外部 MCP 工具源（不拥有其生命周期）
    std::vector<std::string> mcp_tool_names_;  // 经 MCP 接入的工具名（dispatch 据此转发）

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

        // MCP 接入点①：把外部 server 动态发现到的工具，转成本地 ToolSpec 并入 tools_。
        // 这些工具从此和本地工具一视同仁——自动进入 tools_to_json_schema 声明给模型、
        // 走同样的 run_tool 闸门。harness 无需知道它们其实住在另一个进程里。
        if (mcp_ && mcp_->ok()) {
            for (const auto &mt : mcp_->discover()) {
                std::vector<std::pair<std::string, std::string>> schema;
                for (const auto &p : mt.params)
                    schema.push_back({p.first, p.second ? "str" : "str='.'"});  // 必填/可选
                std::string desc = "[via MCP] " + mt.description;
                tools_.push_back({mt.name, schema, false, desc});  // MCP 工具默认非危险
                mcp_tool_names_.push_back(mt.name);
            }
        }
    }

    // is_mcp_tool: 判断某工具名是否是经 MCP 接入的（dispatch 用它决定是否转发给 server）。
    bool is_mcp_tool(const std::string &name) const {
        for (auto &n : mcp_tool_names_) if (n == name) return true;
        return false;
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
    // 组件 2：Prompt Shape —— 静态前缀 + 动态 messages
    // =======================================================================
    // build_prefix: 拼出「不变的」prompt 前缀（角色 + 少量规则 + 工作区快照）。
    // 注意：function calling 下，工具清单与调用格式不再写进 prompt——它们通过 API 的 tools
    // 参数以 JSON Schema 声明（见 tools_to_json_schema）。因此这里既没有工具列表，也没有
    // <tool>/<final> 格式说明，前缀显著变短。只在构造时调用一次，利于模型侧 KV-cache 复用。
    std::string build_prefix() const {
        std::ostringstream o;
        o << "You are Mini-Coding-Agent, a small local coding agent.\n\n"
          << "Rules:\n"
          << "- Use the provided tools instead of guessing about the workspace.\n"
          << "- Never invent tool results. Keep answers concise.\n"
          << "- Once you have enough information, stop calling tools and answer directly.\n\n"
          << ws_.text();//把工作区快照放进来
        return o.str();
    }

    // request_messages: 每轮构造原生 messages 数组；system 放静态前缀和蒸馏记忆，历史保持结构化。
    std::vector<HistoryItem> request_messages() const {
        std::vector<HistoryItem> messages;
        messages.push_back({"system", "", {}, prefix_ + "\n\n" + memory_text(), "", "", {}});
        std::vector<HistoryItem> reduced = reduced_history();
        messages.insert(messages.end(), reduced.begin(), reduced.end());
        return messages;
    }

    // synthesize_after_step_limit: 工具预算耗尽时，禁用工具并要求模型基于已有观察结果直接收尾。
    std::string synthesize_after_step_limit() {
        std::vector<HistoryItem> messages = request_messages();
        messages.push_back({
            "user", "", {},
            "Tool call budget is exhausted. Do not call tools. "
            "Answer the latest user request directly using only the information already shown in the conversation. "
            "If some details are uncertain, state the uncertainty briefly.",
            "", now(), {}
        });
        ModelReply r = model_.complete(messages, {}, 768);
        if (r.content.empty()) return "Stopped after reaching the step limit without a final answer.";
        return r.content;
    }

    // =======================================================================
    // 组件 4：Context Reduction —— 历史瘦身
    // =======================================================================
    // reduced_history: 对结构化历史做瘦身；保留 assistant/tool 配对，只裁剪较早内容长度。
    std::vector<HistoryItem> reduced_history() const {
        std::vector<HistoryItem> out;
        if (history_.empty()) return out;
        size_t recent_start = history_.size() > 6 ? history_.size() - 6 : 0;

        for (size_t i = 0; i < history_.size(); ++i) {
            const auto &it = history_[i];
            bool recent = i >= recent_start;

            size_t limit = recent ? 900 : 180;  // 越近的历史给越大额度
            HistoryItem kept = it;
            if (kept.role != "assistant" || kept.name.empty())
                kept.content = clip(kept.content, limit);
            out.push_back(kept);
        }
        return out;
    }

    // history_text: 把历史序列化成文本，供日志、摘要与 delegate 背景使用；主模型路径不再依赖它。
    std::string history_text() const {
        std::vector<HistoryItem> items = reduced_history();
        if (items.empty()) return "- empty";
        std::vector<std::string> lines;
        for (const auto &it : items) {
            if (it.role == "assistant" && !it.tool_calls.empty()) {
                for (const ToolCall &call : it.tool_calls)
                    lines.push_back("[assistant.tool_call:" + call.name + "] " + args_to_string(call.args));
            } else if (it.role == "tool") {
                lines.push_back("[tool:" + it.name + "] " + args_to_string(it.args));
                lines.push_back(it.content);
            } else {
                lines.push_back("[" + it.role + "] " + it.content);
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
        // MCP 接入点②：本地没有的工具名，若来自 MCP 就通过 JSON-RPC 转发给外部 server。
        // 这就是那个原本硬编码的 if-else 链的「兜底外部源」——工具在进程外，运行时才接入。
        if (is_mcp_tool(name)) {
            if (!mcp_ || !mcp_->ok()) throw std::runtime_error("mcp server not available");
            return mcp_->call(name, args);
        }
        throw std::runtime_error("no dispatch");
    }

    // =======================================================================
    // 说明：文本协议版这里曾有 parse / parse_tool / extract 三个函数，
    // 负责从模型自由文本里抠出 <tool>/<final> 并做容错。改用 function calling 后，
    // 模型直接返回结构化的工具调用（tool_calls），这整套解析与容错逻辑全部消失——
    // 解读工作前移到 RemoteModelClient::complete 里读取结构化字段即可。
    // =======================================================================

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
        // 容错：模型常把 start/end 填成 0 或倒序。夹到合法范围而非直接拒绝，
        // 让"读文件"这种无副作用操作尽量成功，符合"错误反馈应引导而非阻断"的原则。
        if (start < 1) start = 1;
        if (end < start) end = start + 200;
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

#endif  // MINI_AGENT_HPP
