#include "ui/quality_page.h"

#include "rag/retriever.h"
#include "rag/golden_queries.h"
#include "rag/eval_metrics.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/// 给按钮打 role 属性，配色交给全局 QSS 的属性选择器（与设置页同款）
void setButtonRole(QPushButton* button, const char* role) {
    button->setProperty("role", QString::fromUtf8(role));
}

/// 构造一张卡片：card 外框 + cardTitle 标题，返回卡片内部布局
QVBoxLayout* makeCard(QWidget* parent, QVBoxLayout* root, const QString& title) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("card"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
    cardLayout->addWidget(titleLabel);

    auto* body = new QWidget(card);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(16, 12, 16, 12);
    bodyLayout->setSpacing(10);
    cardLayout->addWidget(body, 1);

    root->addWidget(card, 1);
    return bodyLayout;
}

/// 单结果行文本：docId#块号（相关度 X.XX）
QString resultLine(const rag::SearchResult& r) {
    return QStringLiteral("%1#%2（相关度 %3）")
        .arg(QString::fromStdString(r.docId))
        .arg(r.chunkIndex)
        .arg(r.finalScore, 0, 'f', 2);
}

}  // namespace

QualityPage::QualityPage(rag::Retriever* retriever, QWidget* parent)
    : QWidget(parent), retriever_(retriever) {
    setupUi();
}

void QualityPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    // ── 卡片一：单查询四路对比 ──
    auto* compareBody = makeCard(this, root, QStringLiteral("单查询四路并列对比"));
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(10);
        queryInput_ = new QLineEdit(this);
        queryInput_->setObjectName(QStringLiteral("qualityQueryInput"));
        queryInput_->setPlaceholderText(QStringLiteral("输入查询，四路同时检索（Top 10）…"));
        queryInput_->setMinimumHeight(34);
        compareBtn_ = new QPushButton(QStringLiteral("对比检索"), this);
        compareBtn_->setObjectName(QStringLiteral("qualityRunBtn"));
        setButtonRole(compareBtn_, "primary");
        compareBtn_->setMinimumHeight(34);
        row->addWidget(queryInput_, 1);
        row->addWidget(compareBtn_);
        compareBody->addLayout(row);

        compareStatus_ = new QLabel(this);
        compareStatus_->setObjectName(QStringLiteral("qualityStatus"));
        compareStatus_->setWordWrap(true);
        compareBody->addWidget(compareStatus_);

        // 四列结果
        auto* cols = new QHBoxLayout();
        cols->setSpacing(10);
        struct Col { const char* title; QListWidget** field; const char* objectName; };
        const Col colsDef[] = {
            {"① BM25 单路",    &listBm25_,     "qualityListBm25"},
            {"② 向量单路",     &listVector_,   "qualityListVector"},
            {"③ 加权融合",     &listWeighted_, "qualityListWeighted"},
            {"④ RRF 融合",     &listRrf_,      "qualityListRrf"},
        };
        for (const auto& def : colsDef) {
            auto* colBox = new QVBoxLayout();
            auto* colTitle = new QLabel(QString::fromUtf8(def.title), this);
            colTitle->setObjectName(QStringLiteral("qualityColTitle"));
            colBox->addWidget(colTitle);
            auto* list = new QListWidget(this);
            list->setObjectName(QString::fromUtf8(def.objectName));
            list->setMinimumHeight(180);
            colBox->addWidget(list, 1);
            cols->addLayout(colBox, 1);
            *def.field = list;
        }
        compareBody->addLayout(cols, 1);

        connect(compareBtn_, &QPushButton::clicked, this, &QualityPage::onCompare);
        connect(queryInput_, &QLineEdit::returnPressed, this, &QualityPage::onCompare);
    }

    // ── 卡片二：批量评测 ──
    auto* evalBody = makeCard(this, root, QStringLiteral("标注查询集批量评测（23 条 qrels × 4 路）"));
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(10);
        evalBtn_ = new QPushButton(QStringLiteral("批量评测"), this);
        evalBtn_->setObjectName(QStringLiteral("qualityEvalBtn"));
        setButtonRole(evalBtn_, "primary");
        evalBtn_->setMinimumHeight(34);
        row->addWidget(evalBtn_);
        row->addStretch(1);
        evalBody->addLayout(row);

        metricsTable_ = new QTableWidget(4, 5, this);
        metricsTable_->setObjectName(QStringLiteral("qualityMetricsTable"));
        metricsTable_->setHorizontalHeaderLabels({QStringLiteral("通路"),
                                                  QStringLiteral("P@5"),
                                                  QStringLiteral("Hit@5"),
                                                  QStringLiteral("R@10"),
                                                  QStringLiteral("MRR")});
        metricsTable_->verticalHeader()->setVisible(false);
        metricsTable_->horizontalHeader()->setStretchLastSection(true);
        metricsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        metricsTable_->setSelectionMode(QAbstractItemView::NoSelection);
        metricsTable_->setMinimumHeight(190);
        evalBody->addWidget(metricsTable_, 1);

        evalStatus_ = new QLabel(this);
        evalStatus_->setObjectName(QStringLiteral("qualityEvalStatus"));
        evalStatus_->setWordWrap(true);
        evalBody->addWidget(evalStatus_);

        connect(evalBtn_, &QPushButton::clicked, this, &QualityPage::onBatchEval);
    }

    serviceHint_ = new QLabel(this);
    serviceHint_->setObjectName(QStringLiteral("qualityServiceHint"));
    serviceHint_->setWordWrap(true);
    root->addWidget(serviceHint_);

    refreshServiceHint();
}

void QualityPage::refreshServiceHint() {
    if (!retriever_) {
        serviceHint_->setText(QStringLiteral("⚠ 未注入检索引擎"));
        return;
    }
    if (retriever_->embeddingReady()) {
        serviceHint_->setText(
            QStringLiteral("向量路已就绪：%1@%2 —— 四路均为真实结果；"
                           "更换模型后无需重新导入，下次检索自动按新模型重算向量。")
                .arg(QString::fromStdString(retriever_->embeddingModel()),
                     QString::fromStdString(retriever_->embeddingHost())));
    } else {
        serviceHint_->setText(
            QStringLiteral("⚠ Embedding 未配置：向量单路无结果，两路融合自动降级纯 BM25"
                           "（配置见「设置」页 Embedding 服务卡片）。"));
    }
}

void QualityPage::fillList(QListWidget* list,
                           const std::vector<rag::SearchResult>& results,
                           bool vectorMissing) {
    list->clear();
    if (results.empty()) {
        if (vectorMissing) {
            list->addItem(QStringLiteral("（向量路未配置，无结果）"));
        } else {
            list->addItem(QStringLiteral("（无结果）"));
        }
        return;
    }
    for (const auto& r : results) {
        list->addItem(resultLine(r));
    }
}

// ── P0-2 忙碌状态机 ──
// 本页动作（对比/评测）期间禁用两个按钮；跨页互斥由 MainWindow 回灌 setExternalBusy。
// 旧实现对比按钮从不禁用、靠 processEvents 泵事件刷新——重入保护为零（体检 P0-2）。
void QualityPage::refreshActionButtons() {
    const bool enabled = !busySelf_ && !busyExternal_;
    compareBtn_->setEnabled(enabled);
    evalBtn_->setEnabled(enabled);
}

void QualityPage::onCompare() {
    const QString q = queryInput_->text().trimmed();
    if (q.isEmpty() || !retriever_) {
        return;
    }
    if (busySelf_ || busyExternal_) {
        compareStatus_->setText(QStringLiteral("引擎忙碌中，请等待当前任务完成"));
        return;
    }
    beginEngineTask();

    constexpr int kTop = 10;
    compareStatus_->setText(QStringLiteral("四路检索中…"));
    emit compareRequested(q, kTop);   // P1：四路检索在引擎线程执行
}

void QualityPage::onCompareFinished(const ui_engine::CompareResult& result) {
    const bool vectorMissing = !retriever_->embeddingReady();
    fillList(listBm25_, result.bm25, false);
    fillList(listVector_, result.vector, vectorMissing);
    fillList(listWeighted_, result.weighted, false);
    fillList(listRrf_, result.rrf, false);

    endEngineTask();
    if (vectorMissing) {
        compareStatus_->setText(
            QStringLiteral("Embedding 未配置：② 向量单路无结果，③④ 融合路已自动降级纯 BM25。"));
    } else {
        compareStatus_->setText(
            QStringLiteral("完成：BM25 %1 条 / 向量 %2 条 / 加权 %3 条 / RRF %4 条。")
                .arg(result.bm25.size()).arg(result.vector.size())
                .arg(result.weighted.size()).arg(result.rrf.size()));
    }
}

void QualityPage::onBatchEval() {
    if (!retriever_) {
        return;
    }
    if (busySelf_ || busyExternal_) {
        evalStatus_->setText(QStringLiteral("引擎忙碌中，请等待当前任务完成"));
        return;
    }
    beginEngineTask();

    // P1：评测在引擎线程逐条执行（不再 processEvents 手泵），进度经信号回传
    evalStatus_->setText(QStringLiteral("评测中…"));
    emit evalRequested(50);   // 评测宽度：文档级排名取 Top 10 指标，检索放宽到 50
}

void QualityPage::onEvalProgress(int done, int total, const QString& query) {
    evalStatus_->setText(QStringLiteral("评测中… 第 %1 / %2 条：%3").arg(done).arg(total).arg(query));
}

void QualityPage::onEvalRow(int row, const QString& name,
                            double p5, double hit5, double r10, double mrr) {
    metricsTable_->setRowCount(std::max(metricsTable_->rowCount(), row + 1));
    auto put = [this](int row, int col, const QString& text) {
        metricsTable_->setItem(row, col, new QTableWidgetItem(text));
    };
    put(row, 0, name);
    put(row, 1, QString::number(p5, 'f', 3));
    put(row, 2, QString::number(hit5, 'f', 3));
    put(row, 3, QString::number(r10, 'f', 3));
    put(row, 4, QString::number(mrr, 'f', 3));
}

void QualityPage::onEvalFinished(bool vectorMissing) {
    endEngineTask();
    evalStatus_->setText(vectorMissing
        ? QStringLiteral("评测完成（Embedding 未配置：② 向量单路无结果，③④ 为降级 BM25 排名，"
                         "其指标只反映 BM25 排序路径，不具对比意义）。")
        : QStringLiteral("评测完成：23 条标注查询 × 4 路，文档级排名，宏平均。"));
}
