#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QUrl>
#include <functional>
#include <memory>

/// HTTP 传输抽象（P2 接口注入）——Generator / EmbeddingService 的网络出口。
///
/// 设计要点：
///   - 回调在 post() 的调用线程上投递，由该线程的事件循环驱动
///     （引擎线程 / 测试线程）；调用方在事件循环中等待完成。
///   - onData 可被调用 0..N 次（SSE 增量流式）；onFinished **恰好一次**
///     （成功、HTTP 错误、网络错误、被取消，都算一次结束）。
///   - cancel() 仅允许与 post() 同线程调用（引擎线程内中断当前请求）。
///   - 返回的句柄析构不等于取消；调用方若需中断必须显式 cancel()。

/// 一次传输的最终结果（onFinished 恰好携带一次）
struct HttpResponse {
    int statusCode = 0;        // HTTP 状态码；网络层失败时为 0
    QByteArray body;           // 响应体（含 onData 之外的最后残余）
    QString errorText;         // 网络层错误描述（networkError 为真时有意义）
    bool networkError = false; // 传输层失败（连接/超时/被取消）
};

/// 传输句柄：持有方（Generator）借此中断进行中的请求
class ITransportHandle {
public:
    virtual ~ITransportHandle() = default;
    virtual void cancel() = 0;
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    virtual std::shared_ptr<ITransportHandle> post(
        const QUrl& url,
        const QList<QPair<QByteArray, QByteArray>>& headers,
        const QByteArray& body,
        int timeoutMs,
        std::function<void(const QByteArray&)> onData,
        std::function<void(const HttpResponse&)> onFinished) = 0;
};
