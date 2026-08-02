#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

namespace document {

/// 中文分词器（封装 cppjieba）
class Tokenizer {
public:
    Tokenizer();
    ~Tokenizer();

    /// 分词 + 去停用词，返回词语列表
    std::vector<std::string> cut(const std::string& text);

    /// 只保留有意义的关键词（去停用词 + 单字过滤）
    std::vector<std::string> cutForIndex(const std::string& text);

    /// 加载自定义停用词表
    void loadStopWords(const std::vector<std::string>& words);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::unordered_set<std::string> stopWords_;
};

} // namespace document
