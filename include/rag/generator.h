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

    /// 设置检索到的文档元数据摘要（用于生成法律专用 prompt）
    void setMetadataContext(const std::string& metaCtx) { metaContext_ = metaCtx; }

private:
    /// 构建 RAG prompt（自动检测法律/通用场景）
    static std::string buildPrompt(const std::string& query, const std::string& context,
                                   const std::string& metaContext = "");

    /// 构建法律专用 prompt
    static std::string buildLegalPrompt(const std::string& query, const std::string& context,
                                        const std::string& metaContext);

    /// 构建通用 prompt
    static std::string buildGeneralPrompt(const std::string& query, const std::string& context);

    /// 检测是否为法律相关查询
    static bool isLegalContext(const std::string& context);

    /// 获取对应的 system prompt
    static std::string getSystemPrompt(bool isLegal);

    /// 解析 SSE 流中的 JSON delta
    static std::string parseDelta(const std::string& jsonLine);

    std::string apiKey_;
    std::string apiBaseUrl_ = "https://api.deepseek.com";
    std::string metaContext_;  // 文档元数据摘要
};

} // namespace rag
