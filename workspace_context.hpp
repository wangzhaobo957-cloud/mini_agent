// workspace_context.hpp
// ---------------------------------------------------------------------------
// 组件 1：Live Repo Context —— 开局收集一次的稳定环境事实。
// 启动时跑几条 git 命令 + 读取项目文档，拼出一份静态工作区快照，注入 prompt 前缀。
// ---------------------------------------------------------------------------
#ifndef WORKSPACE_CONTEXT_HPP
#define WORKSPACE_CONTEXT_HPP

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "text_utils.hpp"  // clip / trim / sh_capture

namespace fs = std::filesystem;

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

#endif  // WORKSPACE_CONTEXT_HPP
