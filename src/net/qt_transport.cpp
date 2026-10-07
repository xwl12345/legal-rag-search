#include "net/qt_transport.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>

namespace {

/// 句柄：Generator::cancel() 的作用目标（同线程调用 reply->abort()）
class QtHandle : public ITransportHandle {
public:
    explicit QtHandle(QNetworkReply* reply) : reply_(reply) {}
    void cancel() override {
        if (reply_) {
            reply_->abort();
        }
    }

private:
    QPointer<QNetworkReply> reply_;   // reply 结束即销毁，自动置空
};

}  // namespace

QNetworkAccessManager* QtTransport::nam() {
    if (!nam_ || nam_->thread() != QThread::currentThread()) {
        // 懒创建 + 亲和校正：保证应答回调投递在使用线程（引擎线程 / 测试线程）
        nam_ = new QNetworkAccessManager();
    }
    return nam_;
}

std::shared_ptr<ITransportHandle> QtTransport::post(
    const QUrl& url,
    const QList<QPair<QByteArray, QByteArray>>& headers,
    const QByteArray& body,
    int timeoutMs,
    std::function<void(const QByteArray&)> onData,
    std::function<void(const HttpResponse&)> onFinished)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    for (const auto& h : headers) {
        request.setRawHeader(h.first, h.second);
    }
    if (timeoutMs > 0) {
        request.setTransferTimeout(timeoutMs);
    }

    QNetworkReply* reply = nam()->post(request, body);
    auto handle = std::make_shared<QtHandle>(reply);

    // 接收者上下文挂 reply 自身：reply 销毁即断连，无悬挂回调（P0-8 的结构化版本）
    QObject::connect(reply, &QNetworkReply::readyRead, reply,
                     [reply, onData]() {
                         if (onData) {
                             onData(reply->readAll());
                         }
                     });
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     [reply, onData, onFinished]() {
                         // 流尾残余（可能是不完整 SSE 行）先交给 onData，
                         // 再统一收场——调用方按"onFinished 恰好一次"处理
                         if (onData) {
                             const QByteArray rest = reply->readAll();
                             if (!rest.isEmpty()) {
                                 onData(rest);
                             }
                         }
                         HttpResponse resp;
                         resp.statusCode = reply->attribute(
                             QNetworkRequest::HttpStatusCodeAttribute).toInt();
                         if (reply->error() != QNetworkReply::NoError) {
                             resp.networkError = true;
                             resp.errorText = reply->errorString();
                         }
                         resp.body = reply->readAll();
                         reply->deleteLater();
                         if (onFinished) {
                             onFinished(resp);
                         }
                     });

    return handle;
}
