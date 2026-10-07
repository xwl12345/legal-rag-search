#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <vector>

#include "rag/retriever.h"
#include "rag/generator.h"
#include "config/app_settings.h"

class QThread;

namespace ui_engine {

/// 一次批量导入的汇总结果（引擎线程 → UI 线程经队列化信号传递）
struct ImportSummary {
    int imported = 0;
    int chunksAdded = 0;
    int ocrImported = 0;
    bool userCancelled = false;
    int remaining = 0;    // 取消时未处理的文件数
    QStringList errors;   // 「文件名：原因」
};

/// 单查询四路并列对比的完整结果（T4 质量分析页）
struct CompareResult {
    std::vector<rag::SearchResult> bm25;
    std::vector<rag::SearchResult> vector;
    std::vector<rag::SearchResult> weighted;
    std::vector<rag::SearchResult> rrf;
};

}  // namespace ui_engine

Q_DECLARE_METATYPE(ui_engine::ImportSummary)
Q_DECLARE_METATYPE(ui_engine::CompareResult)
Q_DECLARE_METATYPE(std::vector<rag::SearchResult>)

/// 引擎工作线程宿主（P1 线程模型）。
///
/// 职责与线程约定：
///   - 本对象 moveToThread 到专属引擎线程，**重操作**（检索 / 导入 / 生成 /
///     四路对比 / 批量评测 / 配置热更新）全部以队列化槽在这里串行执行；
///     槽内部的网络等待（embed/generate 的事件泵）只阻塞引擎线程，UI 永不卡。
///   - 轻只读（文档列表 / 元数据 / 计数）不经本对象——页面直接调 Retriever
///     的加锁只读口（Retriever 内部短临界区，见 retriever.h 线程模型说明）。
///   - Retriever / Generator 由本对象持有（unique 语义成员），生命周期与
///     引擎线程一致；MainWindow 负责线程的启动与收尾（quit + wait）。
///   - 与旧实现的分工差异：删除 / 清空 / 落盘 / 恢复是纯内存或小文件操作，
///     保留在 UI 线程同步执行（锁保护 + 无网络等待），避免给关闭流程引入
///     异步状态机——属于方案 C 的落地取舍，已记录在优化方案 P1 章节。
class EngineWorker : public QObject {
    Q_OBJECT

public:
    explicit EngineWorker(QObject* parent = nullptr);

    /// 引擎实例指针（轻只读口；MainWindow 构造页面时注入用）。
    /// Retriever 在本对象构造时创建（UI 线程，词典加载与旧行为一致）。
    rag::Retriever* retriever() { return &retriever_; }

    /// 登记导入取消令牌。**UI 线程直调**（先于 importDocuments 入队），
    /// happens-before 由随后队列化事件的投递顺序保证。
    void beginImport(std::shared_ptr<std::atomic_bool> cancelFlag) {
        importCancel_ = std::move(cancelFlag);
    }

public slots:
    // ── 重操作（队列化到引擎线程）──
    void search(const QString& query, int topK);
    void compareModes(const QString& query, int topK);
    void runBatchEval(int width);
    void importDocuments(const QStringList& files);
    void generateAnswer(const QString& query, const QString& context,
                        const QString& metaContext, double temperature);
    void cancelGeneration();          // 与 generateAnswer 同线程，直接 abort
    void applySettings(config::AppSettings settings);
    void setLlmApiKey(const QString& key);

signals:
    // ── 结果回传（队列化到 UI 线程）──
    void searchFinished(const std::vector<rag::SearchResult>& results,
                        const QString& query);
    void compareFinished(const ui_engine::CompareResult& result);
    void evalProgress(int done, int total, const QString& query);
    void evalRow(int row, const QString& name,
                 double p5, double hit5, double r10, double mrr);
    void evalFinished(bool vectorMissing);
    void importProgress(int done, int total, const QString& fileName);
    void importOcrPage(int page, int total);
    void importFinished(const ui_engine::ImportSummary& summary);
    void generationDelta(const QString& text);
    void generationFinished(bool interrupted, const QString& errorText);
    void settingsApplied();

private:
    rag::Retriever retriever_;
    rag::Generator generator_;
    QString llmKey_;
    std::shared_ptr<std::atomic_bool> importCancel_;
};
