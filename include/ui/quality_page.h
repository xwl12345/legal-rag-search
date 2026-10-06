#pragma once
#include <QWidget>

#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

namespace rag {
class Retriever;
struct SearchResult;
}

/// 检索质量分析页（T4）：面向系统维护者的可靠性验证看板。
///
/// 定位（2026-09-20 定稿）：**不是面向最终用户的功能**——语料或 Embedding
/// 模型变更后，在这里重新验证检索质量、调优参数。开题报告中写在
/// "研究内容/实验方案"，不占"主要功能"清单。
///
/// 功能：
///   ① 单查询四路并列对比：BM25 单路 / 向量单路 / 加权融合 / RRF 融合；
///   ② 批量评测：23 条标注查询 × 4 路全部跑一遍，输出 Hit@5 / R@10 / MRR
///      对比指标表（宏平均，文档级排名）。
///
/// 解耦约定（全局约束第 8 条）：对 Retriever **只读调用**（searchWithMode），
/// 不写索引、不发指令；删掉本页 = 去掉注册几行 + 删页面文件，检索功能零影响。
class QualityPage : public QWidget {
    Q_OBJECT

public:
    explicit QualityPage(rag::Retriever* retriever, QWidget* parent = nullptr);

    /// 本页是否有引擎任务进行中（对比 / 批量评测），供 closeEvent 与 E2E 断言
    bool isBusy() const { return busySelf_; }

public slots:
    /// 引擎状态可能变化（如设置页新配了 Key）时刷新提示行
    void refreshServiceHint();

    /// 其他页面有引擎任务进行中时，禁用本页动作按钮（P0-2 跨页互斥，
    /// 由 MainWindow 中转——页面之间不互相引用）
    void setExternalBusy(bool busy) {
        if (busyExternal_ != busy) {
            busyExternal_ = busy;
            refreshActionButtons();
        }
    }

signals:
    /// 引擎任务开始/结束（P0-2）：MainWindow 据此让其他页面禁用引擎动作
    void engineBusyChanged(bool busy);

private slots:
    void onCompare();     // ① 单查询四路对比
    void onBatchEval();   // ② 批量评测 23 条标注查询

private:
    void setupUi();

    /// ── P0-2 忙碌状态机 ──
    void beginEngineTask() {
        busySelf_ = true;
        refreshActionButtons();
        emit engineBusyChanged(true);
    }
    void endEngineTask() {
        busySelf_ = false;
        refreshActionButtons();
        emit engineBusyChanged(false);
    }
    /// 按当前忙碌状态刷新本页动作按钮（自忙或他页忙时一律禁用）
    void refreshActionButtons();

    /// 往单列列表填充一路结果；vectorMissing 时显示降级说明
    void fillList(QListWidget* list, const std::vector<rag::SearchResult>& results,
                  bool vectorMissing);

    rag::Retriever* retriever_;   // 非拥有；只读调用

    // ── P0-2 忙碌状态 ──
    bool busySelf_ = false;
    bool busyExternal_ = false;

    // ── 单查询对比 ──
    QLineEdit* queryInput_ = nullptr;
    QPushButton* compareBtn_ = nullptr;
    QLabel* compareStatus_ = nullptr;
    QListWidget* listBm25_ = nullptr;
    QListWidget* listVector_ = nullptr;
    QListWidget* listWeighted_ = nullptr;
    QListWidget* listRrf_ = nullptr;

    // ── 批量评测 ──
    QPushButton* evalBtn_ = nullptr;
    QTableWidget* metricsTable_ = nullptr;
    QLabel* evalStatus_ = nullptr;
    QLabel* serviceHint_ = nullptr;
};
