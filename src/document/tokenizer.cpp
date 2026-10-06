#include "document/tokenizer.h"
#include "cppjieba/Jieba.hpp"
#include <QFile>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace document {

// ── 默认停用词表 ──
static const std::unordered_set<std::string> DEFAULT_STOP_WORDS = {
    "的", "了", "在", "是", "我", "有", "和", "就", "不", "人", "都", "一",
    "一个", "上", "也", "很", "到", "说", "要", "去", "你", "会", "着",
    "没有", "看", "好", "自己", "这", "他", "她", "它", "们", "那", "些",
    "所", "为", "所以", "因为", "但是", "然而", "而且", "虽然", "如果",
    "可以", "还是", "只是", "之后", "然后", "已经", "这个", "那个",
    "什么", "怎么", "吗", "啊", "吧", "呢", "哦", "嗯",
    "the", "a", "an", "is", "are", "was", "were", "be", "been",
    "being", "have", "has", "had", "do", "does", "did", "will",
    "would", "could", "should", "may", "might", "can", "shall",
    "to", "of", "in", "for", "on", "with", "at", "by", "from",
    "it", "its", "and", "or", "but", "not", "no", "this", "that"
};

// ── 词典目录的运行期解析（P0-1）──
// 旧实现把编译期绝对路径写死进二进制，词典缺失时 cppjieba 的 XCHECK 直接
// abort()，异常都接不住——exe 拷到别的机器就是启动即崩。现在按优先级探测：
//   1. 环境变量 LEGAL_RAG_DICT_DIR（部署兜底）
//   2. exe 所在目录 /dict（部署拷贝；CMake POST_BUILD 会随构建复制一份）
//   3. 编译期路径（开发：源码树绝对路径，由 CMake 注入）
//   4. 工作目录相对路径 third_party/cppjieba/dict（开发：从项目根运行）
// 全部落空则抛出带指引的异常（main 捕获后弹对话框），不再裸 abort。
namespace {

std::string trimDirSeparators(std::string dir) {
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
        dir.pop_back();
    }
    return dir;
}

bool dictFileExists(const std::string& dir, const char* file) {
    const std::string base = trimDirSeparators(dir);
    return QFile::exists(QString::fromStdString(base + "/" + file));
}

/// 返回探测成功的词典目录；找不到时抛出带指引的异常。
const std::string& resolveDictDir() {
    static const std::string dir = []() -> std::string {
        std::vector<std::string> tried;

        if (const char* env = std::getenv("LEGAL_RAG_DICT_DIR"); env && *env) {
            if (dictFileExists(env, "jieba.dict.utf8")) {
                return trimDirSeparators(env);
            }
            tried.push_back(std::string("LEGAL_RAG_DICT_DIR=") + env);
        }

#if defined(_WIN32)
        char exePathBuf[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH) > 0) {
            std::string exeDir(exePathBuf);
            const size_t slash = exeDir.find_last_of("/\\");
            if (slash != std::string::npos) {
                exeDir = trimDirSeparators(exeDir.substr(0, slash)) + "/dict";
                if (dictFileExists(exeDir, "jieba.dict.utf8")) {
                    return exeDir;
                }
                tried.push_back(exeDir);
            }
        }
#endif

        if (dictFileExists(CPPJIEBA_DICT_PATH, "jieba.dict.utf8")) {
            return trimDirSeparators(CPPJIEBA_DICT_PATH);
        }
        tried.push_back(CPPJIEBA_DICT_PATH);

        if (dictFileExists("third_party/cppjieba/dict", "jieba.dict.utf8")) {
            return "third_party/cppjieba/dict";
        }
        tried.push_back("third_party/cppjieba/dict");

        std::string hint = "找不到 cppjieba 词典目录，已尝试：";
        for (size_t i = 0; i < tried.size(); ++i) {
            hint += (i ? "；" : "") + tried[i];
        }
        hint += "。请把词典目录拷贝到程序同级 dict/ 下，或设置环境变量 LEGAL_RAG_DICT_DIR";
        throw std::runtime_error(hint);
    }();
    return dir;
}

std::string dictPath(const char* file) {
    const std::string& dir = resolveDictDir();
    return (dir.back() == '/' || dir.back() == '\\') ? dir + file : dir + "/" + file;
}

}  // namespace

// ── Pimpl 封装 cppjieba ──
class Tokenizer::Impl {
public:
    Impl()
        : jieba_(dictPath("jieba.dict.utf8"),
                 dictPath("hmm_model.utf8"),
                 dictPath("user.dict.utf8"),
                 dictPath("idf.utf8"),
                 dictPath("stop_words.utf8"))
    {
    }

    void cut(const std::string& text, std::vector<std::string>& words) {
        jieba_.Cut(text, words, true);
    }

    bool insertUserWord(const std::string& word, int freq, const std::string& tag) {
        return jieba_.InsertUserWord(word, freq, tag);
    }

private:
    cppjieba::Jieba jieba_;
};

// ── Tokenizer ──
Tokenizer::Tokenizer()
    : impl_(std::make_unique<Impl>())
    , stopWords_(DEFAULT_STOP_WORDS)
{
    // 自动加载法律领域词典（与主词典同一运行期解析出的目录）
    int loaded = loadUserDict(dictPath("legal_dict.utf8"));
    if (loaded > 0) {
        // 静默加载成功（避免干扰日志输出）
    }
}

Tokenizer::~Tokenizer() = default;

std::vector<std::string> Tokenizer::cut(const std::string& text) {
    std::vector<std::string> words;
    if (text.empty()) return words;
    impl_->cut(text, words);
    return words;
}

std::vector<std::string> Tokenizer::cutForIndex(const std::string& text) {
    std::vector<std::string> raw;
    impl_->cut(text, raw);

    // 过滤停用词、单字、纯数字、纯标点
    std::vector<std::string> filtered;
    for (auto& w : raw) {
        if (w.empty()) continue;

        // 英文统一转小写（避免 "RAG" vs "rag" 不匹配）
        for (char& c : w) {
            if (static_cast<unsigned char>(c) < 128) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }

        if (stopWords_.count(w)) continue;

        // 过滤单字
        // 注意：中文字符占 3 个字节（UTF-8），英文单个字母占 1 个字节
        // 这里简单判断：UTF-8 中文字符长度 ≥ 3
        if (w.size() < 3) {
            // 可能是英文单词
            bool allAscii = true;
            for (char c : w) {
                if (static_cast<unsigned char>(c) > 127) {
                    allAscii = false;
                    break;
                }
            }
            // 英文单词至少 2 个字符，中文词至少 3 字节（1 个汉字）
            if (allAscii && w.size() < 2) continue;
            if (!allAscii && w.size() < 3) continue;
        }

        // 过滤纯数字
        bool allDigit = true;
        for (char c : w) {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') {
                allDigit = false;
                break;
            }
        }
        if (allDigit) continue;

        filtered.push_back(w);
    }

    return filtered;
}

void Tokenizer::loadStopWords(const std::vector<std::string>& words) {
    for (const auto& w : words) {
        stopWords_.insert(w);
    }
}

int Tokenizer::loadUserDict(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) return 0;

    int count = 0;
    std::string line;
    while (std::getline(file, line)) {
        // 跳过空行和注释
        if (line.empty() || line[0] == '#') continue;

        // 去除首尾空白
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        size_t end = line.size();
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
        if (start >= end) continue;

        std::string trimmed = line.substr(start, end - start);

        // 按空白分隔：word [freq] [tag]
        std::istringstream iss(trimmed);
        std::string word, tag = "l";  // 默认标签：法律术语
        int freq = 10;                 // 默认频率

        if (iss >> word) {
            if (iss >> freq) {
                if (!(iss >> tag)) {
                    tag = "l";
                }
            }
            impl_->insertUserWord(word, freq, tag);
            ++count;
        }
    }

    return count;
}

} // namespace document
