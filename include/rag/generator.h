#pragma once
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "net/transport.h"

namespace rag {

/// AI 生成答案（流式回调）
class Generator {
public:
    /// 回调：每次收到一个文本增量时调用
    using StreamCallback = std::function<void(const std::string& delta)>;

    /// 设置 API Key
    void setApiKey(const std::string& key) { apiKey_ = key; }

    /// 运行时热更新采样温度（T3 配置中心）；默认 0.3，合法范围 (0, 2]
    void setTemperature(double t) { temperature_ = t; }
    double temperature() const { return temperature_; }

    /// 网络传输注入（P2）：默认 QtTransport（首次请求时在当前线程懒创建，
    /// 头文件不引入 QtTransport 完整类型）；测试注入 FakeTransport 走全链路
    void setTransport(std::shared_ptr<IHttpTransport> transport) {
        transport_ = std::move(transport);
    }

    /// 服务地址与模型名（P2 起真正生效——旧实现硬编码且 apiBaseUrl_ 是死成员）
    void setEndpoint(const std::string& baseUrl, const std::string& model) {
        if (!baseUrl.empty()) apiBaseUrl_ = baseUrl;
        if (!model.empty()) chatModel_ = model;
    }
    std::string apiBaseUrl() const { return apiBaseUrl_; }
    std::string chatModel() const { return chatModel_; }

    /// 基于检索到的上下文 + 用户问题，调用 LLM 生成答案
    /// @param query    用户问题
    /// @param context  检索到的上下文（由 Retriever::buildContext 生成）
    /// @param callback 流式输出回调（可选，为 nullptr 时同步返回完整答案）
    /// @return         完整答案文本
    /// @throw std::runtime_error 网络失败 / HTTP 错误 / 200 包错误体且无任何增量
    std::string generate(const std::string& query,
                         const std::string& context,
                         StreamCallback callback = nullptr);

    /// 中断当前 generate()：对活动传输句柄 cancel（仅允许与 generate 同线程调用；
    /// 引擎线程内由 EngineWorker::cancelGeneration 触发），等待以网络错误收场
    void cancel();

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

    std::string apiKey_;
    std::string apiBaseUrl_ = "https://api.deepseek.com";
    std::string chatModel_ = "deepseek-chat";
    std::string metaContext_;  // 文档元数据摘要
    double temperature_ = 0.3;  // 采样温度（T3 配置中心）

    // P2：网络出口可注入；空 = 未注入，generate() 首次使用时懒创建 QtTransport
    std::shared_ptr<IHttpTransport> transport_;
    std::shared_ptr<ITransportHandle> activeHandle_;
    bool cancelRequested_ = false;   // P3：用户主动停止 → generate 恒抛中断（不伪装成完整回答）
};

} // namespace rag
