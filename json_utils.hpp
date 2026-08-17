// json_utils.hpp
// ---------------------------------------------------------------------------
// HTTP/JSON 辅助 —— 供远程模型客户端拼请求、解析响应、保护 shell 参数。
// 这里是一套极小的手写 JSON 处理（编码 / 反转义 / 浅解析），不引第三方库；
// 只覆盖 agent 循环里会遇到的扁平结构，够用即可。
// ---------------------------------------------------------------------------
#ifndef JSON_UTILS_HPP
#define JSON_UTILS_HPP

#include <cstdio>
#include <map>
#include <string>

#include "text_utils.hpp"  // trim

// json_quote: 把任意字符串编码成一个合法的 JSON 字符串字面量（含首尾双引号）。
// 转义引号、反斜杠、换行等控制字符，否则把多行 prompt 塞进 JSON body 会破坏结构。
inline std::string json_quote(const std::string &s) {
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

// flat_json_object: 把 string map 编码成一个扁平 JSON 对象（用于 function.arguments）。
inline std::string flat_json_object(const std::map<std::string, std::string> &obj) {
    std::string out = "{";
    size_t i = 0;
    for (const auto &kv : obj) {
        out += json_quote(kv.first) + ":" + json_quote(kv.second);
        if (++i < obj.size()) out += ",";
    }
    out += "}";
    return out;
}

// shell_quote: 把字符串包成单引号安全的 shell 参数（用于把 JSON body 传给 curl -d）。
// 单引号内一切原样，遇到内部的单引号用 '\'' 收尾再拼回，避免命令注入与解析错乱。
inline std::string shell_quote(const std::string &s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
}

// json_unescape: 把 JSON 字符串字面量里的转义序列还原成原始文本（extract 的配套）。
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
inline std::string extract_json_string(const std::string &json, const std::string &key) {
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

// parse_flat_json_object: 把扁平 JSON 对象（如 {"path":"README.md","start":1}）解析成 string map。
// 用于解读 tool_calls 里 function.arguments 那段 JSON——参数值统一转成字符串，交给现有工具实现。
inline std::map<std::string, std::string> parse_flat_json_object(const std::string &obj) {
    std::map<std::string, std::string> out;
    size_t b = obj.find('{');
    if (b == std::string::npos) return out;
    size_t i = b + 1;
    auto skip_ws = [&]() { while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\n' ||
                                                      obj[i] == '\t' || obj[i] == '\r')) ++i; };
    while (i < obj.size()) {
        skip_ws();
        if (i >= obj.size() || obj[i] == '}') break;
        if (obj[i] != '"') break;               // 键必须是字符串
        std::string key_raw; ++i;
        for (; i < obj.size(); ++i) {
            if (obj[i] == '\\') { key_raw += obj[i]; if (++i < obj.size()) key_raw += obj[i]; continue; }
            if (obj[i] == '"') break;
            key_raw += obj[i];
        }
        ++i; skip_ws();
        if (i < obj.size() && obj[i] == ':') ++i;
        skip_ws();
        std::string val;
        if (i < obj.size() && obj[i] == '"') {  // 字符串值：保留转义后统一反转义
            std::string raw2; ++i;
            for (; i < obj.size(); ++i) {
                if (obj[i] == '\\') { raw2 += obj[i]; if (++i < obj.size()) raw2 += obj[i]; continue; }
                if (obj[i] == '"') break;
                raw2 += obj[i];
            }
            ++i; val = json_unescape(raw2);
        } else {                                 // 数字/布尔等裸值
            for (; i < obj.size() && obj[i] != ',' && obj[i] != '}'; ++i) val += obj[i];
            val = trim(val);
        }
        out[key_raw] = val;
        skip_ws();
        if (i < obj.size() && obj[i] == ',') { ++i; continue; }
        break;
    }
    return out;
}

#endif  // JSON_UTILS_HPP
