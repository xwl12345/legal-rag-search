#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace vector_engine {

/// 向量嵌入服务：调用 AI API 将文本转为向量
class EmbeddingService {
public:
    /// 设置 API Key
    void setApiKey(const std::string& key) { apiKey_ = key; }

    /// 将单个文本转为向量
    std::vector<double> embed(const std::string& text);

    /// 批量将多个文本转为向量（一次 API 调用）
    std::vector<std::vector<double>> embedBatch(const std::vector<std::string>& texts);

    /// 检查 API 是否已配置
    bool isReady() const { return !apiKey_.empty(); }

private:
    std::string apiKey_;
    std::string apiBaseUrl_ = "https://api.deepseek.com";
};

} // namespace vector_engine
