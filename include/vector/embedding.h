#pragma once
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "net/transport.h"

namespace vector_engine {

/// 向量嵌入服务：调用 AI API 将文本转为向量
class EmbeddingService {
public:
    /// 设置 API Key
    void setApiKey(const std::string& key) { apiKey_ = key; }

    /// 当前 API Key（供 Retriever 判断「下发值是否真的变化」，避免无谓清缓存）
    std::string apiKey() const { return apiKey_; }

    /// 运行时配置服务地址与模型名（T3 配置中心，T4 消费）。
    /// baseUrl 形如 https://api.siliconflow.cn —— 带 / 或 /v1 结尾也可以，
    /// 程序会先规范化（剥掉尾部 / 与 /v1）再拼 /v1/embeddings，不会双写。
    void setEndpoint(const std::string& baseUrl, const std::string& model) {
        if (!baseUrl.empty()) apiBaseUrl_ = baseUrl;
        if (!model.empty()) model_ = model;
    }
    std::string apiBaseUrl() const { return apiBaseUrl_; }
    std::string model() const { return model_; }

    /// 网络传输注入（P2）：默认 QtTransport（首次请求时在当前线程懒创建，
    /// 头文件不引入 QtTransport 完整类型）；测试注入 FakeTransport 走全链路
    void setTransport(std::shared_ptr<IHttpTransport> transport) {
        transport_ = std::move(transport);
    }

    /// 将单个文本转为向量
    std::vector<double> embed(const std::string& text);

    /// 批量将多个文本转为向量（一次 API 调用）
    /// @throw std::runtime_error 网络失败 / HTTP 错误 / 200 包错误体
    std::vector<std::vector<double>> embedBatch(const std::vector<std::string>& texts);

    /// 检查 API 是否已配置
    bool isReady() const { return !apiKey_.empty(); }

    /// 规范化服务地址：剥掉尾部 '/' 与 '/v1'，供拼 /v1/embeddings 使用。
    /// 用户手填 https://api.siliconflow.cn/ 或 .../v1 都能落到同一个 URL。
    /// 公开以便单测直接断言（纯函数，无副作用）。
    std::string normalizedBaseUrl() const;

private:
    std::string apiKey_;
    std::string apiBaseUrl_ = "https://api.deepseek.com";
    std::string model_ = "text-embedding-3-small";

    // P2：网络出口可注入；空 = 未注入，embedBatch 首次使用时懒创建 QtTransport
    std::shared_ptr<IHttpTransport> transport_;
};

} // namespace vector_engine
