// text_utils.hpp
// ---------------------------------------------------------------------------
// 通用小工具：文本裁剪 / 去空白 / 时间戳 / 执行 shell 抓输出。
// 这些函数与 agent 语义无关，是被各处复用的最底层公共依赖，单独成文件便于查阅。
// ---------------------------------------------------------------------------
#ifndef TEXT_UTILS_HPP
#define TEXT_UTILS_HPP

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

// clip: 把过长文本截断到 limit，并标注被截掉多少字符（对应组件4的输出裁剪）。
inline std::string clip(const std::string &text, size_t limit = 4000) {
    if (text.size() <= limit) return text;
    return text.substr(0, limit) + "\n...[truncated " +
           std::to_string(text.size() - limit) + " chars]";
}

// trim: 去掉字符串首尾空白，解析模型输出时常用。
inline std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// now: 返回一个粗略的时间戳字符串，仅用于给历史条目打标记。
inline std::string now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

// sh_capture: 执行一条 shell 命令并抓取其标准输出（用于收集 git 信息 / run_shell 工具）。
inline std::string sh_capture(const std::string &cmd) {
    std::string out;
    //执行shell命令，获取标准输出
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

#endif  // TEXT_UTILS_HPP
