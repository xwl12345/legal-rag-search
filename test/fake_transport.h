#pragma once
// 可编程假 HTTP 传输（P2 测试注入）——单测与 ui_smoke 离线驱动真实网络链路。
//
// 两种用法：
//   1. 回放模式：enqueueChunk()/enqueueFinish() 预置字节分片，post 后逐步投递；
//   2. 应答器模式：setResponder(fn)——post 时按 (url, body) 现算分片与结束帧。
//
//   setHangNext(true)：下一次 post 不投递任何数据也不收场（模拟"生成卡住"），
//   配合 ITransportHandle::cancel() 可端到端测试「生成中点停止」。
//
// 投递由当前线程事件循环驱动（QTimer 逐步走），onData/onFinished 的线程语义
// 与 QtTransport 一致；cancel() 同步以 networkError 收场（onFinished 恰好一次）。
#include "net/transport.h"

#include <QTimer>
#include <QUrl>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

class FakeTransport : public IHttpTransport {
public:
    struct Finish {
        int statusCode = 200;
        QByteArray body;
        bool networkError = false;
        QString errorText;
    };
    struct Reply {
        std::vector<QByteArray> chunks;
        Finish finish;
    };
    using Responder = std::function<Reply(const QUrl&, const QByteArray&)>;

    FakeTransport() = default;

    // ── 脚本配置（post 之前调用）──
    void enqueueChunk(QByteArray c) {
        steps_.push_back(Step{std::move(c), nullptr});
    }
    void enqueueFinish(Finish f) {
        steps_.push_back(Step{{}, std::make_shared<Finish>(std::move(f))});
    }
    void setResponder(Responder r) { responder_ = std::move(r); }
    void setHangNext(bool h) { hangNext_ = h; }

    // ── IHttpTransport ──
    std::shared_ptr<ITransportHandle> post(
        const QUrl& url,
        const QList<QPair<QByteArray, QByteArray>>& headers,
        const QByteArray& body,
        int timeoutMs,
        std::function<void(const QByteArray&)> onData,
        std::function<void(const HttpResponse&)> onFinished) override
    {
        Q_UNUSED(headers);
        Q_UNUSED(timeoutMs);

        auto pending = std::make_shared<Pending>();
        pending->onData = std::move(onData);
        pending->onFinished = std::move(onFinished);

        if (hangNext_) {
            hangNext_ = false;   // 不投递、不收场：挂起直到 cancel()
        } else if (responder_) {
            const Reply r = responder_(url, body);
            for (const auto& c : r.chunks) {
                pending->steps.push_back(Step{c, nullptr});
            }
            pending->steps.push_back(Step{{}, std::make_shared<Finish>(r.finish)});
        } else {
            pending->steps = steps_;
            steps_.clear();
        }

        auto handle = std::make_shared<FakeHandle>(pending);
        // 逐步投递：每个事件循环 pass 处理一步（流式增量对调用方可见）。
        // 上下文挂 timer（QObject）：timer 随 Pending 析构即断连，lambda 持
        // shared_ptr，无悬挂。
        pending->timer.setSingleShot(false);
        pending->timer.setInterval(0);
        QObject::connect(&pending->timer, &QTimer::timeout, &pending->timer,
                         [pending]() { deliverNext(pending); });
        pending->timer.start();
        return handle;
    }

private:
    struct Step {
        QByteArray chunk;
        std::shared_ptr<Finish> finish;   // 非空 = 结束帧
    };

    struct Pending {
        std::function<void(const QByteArray&)> onData;
        std::function<void(const HttpResponse&)> onFinished;
        std::deque<Step> steps;
        QTimer timer;
        bool finished = false;
    };

    class FakeHandle : public ITransportHandle {
    public:
        explicit FakeHandle(std::shared_ptr<Pending> p) : p_(std::move(p)) {}
        void cancel() override {
            if (p_ && !p_->finished) {
                // 同步收场：cancel 与 post 同线程，等待中的事件循环随即退出
                // （语义与 QtTransport 的 reply->abort() 一致）
                HttpResponse resp;
                resp.networkError = true;
                resp.errorText = QStringLiteral("Operation canceled");
                p_->finished = true;
                p_->timer.stop();
                if (p_->onFinished) {
                    p_->onFinished(resp);
                }
            }
        }

    private:
        std::shared_ptr<Pending> p_;
    };

    static void deliverNext(std::shared_ptr<Pending> p) {
        if (p->finished) {
            p->timer.stop();
            return;
        }
        if (p->steps.empty()) {
            // 脚本耗尽（挂起模式）：静默等待 cancel
            p->timer.stop();
            return;
        }
        Step st = std::move(p->steps.front());
        p->steps.pop_front();
        if (st.finish) {
            p->finished = true;
            p->timer.stop();
            if (p->onData && !st.chunk.isEmpty()) {
                p->onData(st.chunk);
            }
            HttpResponse resp;
            resp.statusCode = st.finish->statusCode;
            resp.body = st.finish->body;
            resp.networkError = st.finish->networkError;
            resp.errorText = st.finish->errorText;
            if (p->onFinished) {
                p->onFinished(resp);
            }
        } else if (p->onData) {
            p->onData(st.chunk);
        }
    }

    std::deque<Step> steps_;
    Responder responder_;
    bool hangNext_ = false;
};
