#pragma once
#include "net/transport.h"

#include <QNetworkAccessManager>
#include <QPointer>

/// 生产传输实现：QNetworkAccessManager 直连。
///
/// 线程注意：QNetworkAccessManager 是 QObject，其应答事件投递到**创建它的线程**。
/// 本类被 Generator/EmbeddingService 持有，使用方是引擎线程（P1），因此 NAM
/// 懒创建：首次 post() 时在当前线程构建；若发现亲和线程与当前不符（不应发生，
/// 防御性兜底）则重建，确保 onData/onFinished 始终在调用线程上投递。
class QtTransport : public IHttpTransport {
public:
    QtTransport() = default;

    std::shared_ptr<ITransportHandle> post(
        const QUrl& url,
        const QList<QPair<QByteArray, QByteArray>>& headers,
        const QByteArray& body,
        int timeoutMs,
        std::function<void(const QByteArray&)> onData,
        std::function<void(const HttpResponse&)> onFinished) override;

private:
    QNetworkAccessManager* nam();

    QPointer<QNetworkAccessManager> nam_;
};
