#pragma once
#include <string>
#include <vector>
#include <functional>

namespace rag {

/// AI 生成答案（流式回调）
class Generator {
public:
    /// 回调：每次收到一个文本增量时调用
    using StreamCallback = std::function<void(const std::string& delta)>;

    /// 设置 API Key
    void setApiKey(const std::string& key) { apiKey_ = key; }

    /// 基于检索到的上下文 + 用户问题，调用 LLM 生成答案
    /// @param query    用户问题
    /// @param context  检索到的上下文（由 Retriever::buildContext 生成）
    /// @param callback 流式输出回调（可选，为 nullptr 时同步返回完整答案）
    /// @return         完整答案文本
    std::string generate(const std::string& query,
                         const std::string& context,
                         StreamCallback callback = nullptr);

    /// 检查 API 是否已配置
    bool isReady() const { return !apiKey_.empty(); }

private:
    /// 构建 RAG prompt
    static std::string buildPrompt(const std::string& query, const std::string& context);

    /// 解析 SSE 流中的 JSON delta
    static std::string parseDelta(const std::string& jsonLine);

    std::string apiKey_;
    std::string apiBaseUrl_ = "https://api.deepseek.com";
};

} // namespace rag
