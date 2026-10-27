#include "ui/settings_page.h"

#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "config/app_config.h"
#include "ui/app_theme.h"

namespace {

/// 统一构造一张参数卡片：card 外框 + cardTitle 标题 + 表单体
QLayout* makeCard(QWidget* parent, QVBoxLayout* root,
                  const QString& title,
                  QFormLayout*& formOut) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("card"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
    cardLayout->addWidget(titleLabel);

    auto* body = new QWidget(card);
    auto* form = new QFormLayout(body);
    form->setContentsMargins(16, 12, 16, 12);
    form->setSpacing(10);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cardLayout->addWidget(body);

    root->addWidget(card);
    formOut = form;
    return cardLayout;
}

QDoubleSpinBox* makeDoubleSpin(QWidget* parent, double min, double max, double step) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(2);
    spin->setMinimumHeight(32);
    spin->setMinimumWidth(140);
    return spin;
}

QSpinBox* makeIntSpin(QWidget* parent, int min, int max) {
    auto* spin = new QSpinBox(parent);
    spin->setRange(min, max);
    spin->setMinimumHeight(32);
    spin->setMinimumWidth(140);
    return spin;
}

QLabel* makeHint(QWidget* parent, const QString& text) {
    auto* hint = new QLabel(text, parent);
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);
    return hint;
}

}  // namespace

SettingsPage::SettingsPage(const config::AppSettings& settings, QWidget* parent)
    : QWidget(parent) {
    setupUi();
    // P3：配置由 MainWindow 注入（组合根已 load 过一次），本页不再自读 JSON——
    // 消灭"两处加载时序间文件变化则状态分叉"的隐患。
    populate(settings);
}

void SettingsPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(10);

    // ── 页头 ──
    auto* head = new QHBoxLayout();
    head->setSpacing(12);
    auto* titleLabel = new QLabel(QStringLiteral("设置"), this);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(
        QStringLiteral("检索参数即时生效 · 配置落盘重启保留"), this);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    head->addWidget(titleLabel);
    head->addWidget(subtitleLabel);
    head->addStretch();
    root->addLayout(head);

    // ── 修复（T6 期）：四张卡片包进滚动区 ──
    // 窗口高度不足以容纳全部表单时，QVBoxLayout 会强压 QFormLayout 行距，
    // 标签与输入框叠印（"小窗正常、放大窗口反而重叠"的实测现象）。
    // 改为内容可滚：空间不足 = 出滚动条，物理上不再压缩任何表单行。
    auto* scrollContent = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    scrollContent->setAutoFillBackground(false);
    scroll->setWidget(scrollContent);

    // ── 卡片一：检索参数 ──
    QFormLayout* form1 = nullptr;
    makeCard(this, contentLayout, QStringLiteral("检索参数（保存后立即生效，无需重建索引）"), form1);

    k1Spin_ = makeDoubleSpin(this, 0.0, 10.0, 0.1);
    k1Spin_->setObjectName(QStringLiteral("settingsK1"));
    bSpin_ = makeDoubleSpin(this, 0.0, 1.0, 0.05);
    bSpin_->setObjectName(QStringLiteral("settingsB"));
    bm25WeightSpin_ = makeDoubleSpin(this, 0.0, 1.0, 0.05);
    bm25WeightSpin_->setObjectName(QStringLiteral("settingsBm25Weight"));
    vectorWeightSpin_ = makeDoubleSpin(this, 0.0, 1.0, 0.05);
    vectorWeightSpin_->setObjectName(QStringLiteral("settingsVecWeight"));
    topKSpin_ = makeIntSpin(this, 1, 200);
    topKSpin_->setObjectName(QStringLiteral("settingsTopK"));
    temperatureSpin_ = makeDoubleSpin(this, 0.0, 2.0, 0.05);
    temperatureSpin_->setObjectName(QStringLiteral("settingsTemperature"));

    form1->addRow(QStringLiteral("BM25 k1（词频饱和）"), k1Spin_);
    form1->addRow(QStringLiteral("BM25 b（长度归一化）"), bSpin_);
    form1->addRow(QStringLiteral("BM25 权重"), bm25WeightSpin_);
    form1->addRow(QStringLiteral("向量权重"), vectorWeightSpin_);
    form1->addRow(QStringLiteral("检索条数 TopK"), topKSpin_);
    form1->addRow(QStringLiteral("生成温度 temperature"), temperatureSpin_);
    form1->addRow(makeHint(this,
        QStringLiteral("TopK = 检索问答页每次返回的候选文本块数（默认 20，与检索页行为一致）；")
            + QStringLiteral("temperature 控制 AI 回答的随机度（默认 0.3）。")));

    // ── 卡片二：分块参数 ──
    QFormLayout* form2 = nullptr;
    makeCard(this, contentLayout, QStringLiteral("分块参数（仅对之后导入的文档生效）"), form2);

    chunkSizeSpin_ = makeIntSpin(this, 64, 4096);
    chunkSizeSpin_->setObjectName(QStringLiteral("settingsChunkSize"));
    chunkOverlapSpin_ = makeIntSpin(this, 0, 1024);
    chunkOverlapSpin_->setObjectName(QStringLiteral("settingsChunkOverlap"));

    form2->addRow(QStringLiteral("块最大字符数"), chunkSizeSpin_);
    form2->addRow(QStringLiteral("相邻块重叠字符数"), chunkOverlapSpin_);
    form2->addRow(makeHint(this,
        QStringLiteral("已导入文档的块边界在导入时已固定：改小参数不会把现有文本块重新切，")
            + QStringLiteral("只影响之后导入的文档；需要全库生效请清空索引后重新导入。")));

    // ── 卡片三：生成（LLM）服务（P3 新增；Key 在检索页，此处只配服务）──
    QFormLayout* formChat = nullptr;
    makeCard(this, contentLayout, QStringLiteral("生成服务（DeepSeek Chat，Key 在检索问答页配置）"), formChat);

    chatUrlEdit_ = new QLineEdit(this);
    chatUrlEdit_->setObjectName(QStringLiteral("settingsChatUrl"));
    chatUrlEdit_->setPlaceholderText(QStringLiteral("https://api.deepseek.com"));
    chatUrlEdit_->setMinimumHeight(32);

    chatModelEdit_ = new QLineEdit(this);
    chatModelEdit_->setObjectName(QStringLiteral("settingsChatModel"));
    chatModelEdit_->setPlaceholderText(QStringLiteral("deepseek-chat"));
    chatModelEdit_->setMinimumHeight(32);

    formChat->addRow(QStringLiteral("服务地址"), chatUrlEdit_);
    formChat->addRow(QStringLiteral("模型名"), chatModelEdit_);
    formChat->addRow(makeHint(this,
        QStringLiteral("模型名随配置保存，下一次生成即生效；OpenAI 兼容端点可换")
            + QStringLiteral("（服务地址 + 模型名成对修改）。")));

    // ── 卡片四：Embedding 服务 ──
    QFormLayout* form3 = nullptr;
    makeCard(this, contentLayout, QStringLiteral("Embedding 服务（检索质量分析 T4 将消费）"), form3);

    embedUrlEdit_ = new QLineEdit(this);
    embedUrlEdit_->setObjectName(QStringLiteral("settingsEmbedUrl"));
    embedUrlEdit_->setPlaceholderText(QStringLiteral("https://api.deepseek.com"));
    embedUrlEdit_->setMinimumHeight(32);

    embedModelEdit_ = new QLineEdit(this);
    embedModelEdit_->setObjectName(QStringLiteral("settingsEmbedModel"));
    embedModelEdit_->setPlaceholderText(QStringLiteral("text-embedding-3-small"));
    embedModelEdit_->setMinimumHeight(32);

    embedKeyEdit_ = new QLineEdit(this);
    embedKeyEdit_->setObjectName(QStringLiteral("settingsEmbedKey"));
    embedKeyEdit_->setPlaceholderText(QStringLiteral("sk-xxxxxxxxxxxxxxxxxxxxxxxx"));
    embedKeyEdit_->setEchoMode(QLineEdit::Password);
    embedKeyEdit_->setMinimumHeight(32);

    form3->addRow(QStringLiteral("服务地址"), embedUrlEdit_);
    form3->addRow(QStringLiteral("模型名"), embedModelEdit_);
    form3->addRow(QStringLiteral("API Key"), embedKeyEdit_);
    form3->addRow(makeHint(this,
        QStringLiteral("Key 留空 = 沿用检索页 / DEEPSEEK_API_KEY 环境变量的既有流程；")
            + QStringLiteral("填写后以此处为准。Key 保存在本地配置文件（rag_settings.json，已加入 .gitignore）。")));

    // ── 卡片五：同义词归一化（T7：口语 → 规范术语，BM25 查询扩展）──
    QFormLayout* formSyn = nullptr;
    makeCard(this, contentLayout,
             QStringLiteral("同义词归一化（口语查询 → 规范术语，保存后立即生效）"), formSyn);

    synonymTable_ = new QTableWidget(0, 2, this);
    synonymTable_->setObjectName(QStringLiteral("settingsSynonymTable"));
    synonymTable_->setHorizontalHeaderLabels({QStringLiteral("口语词（查询中包含即扩展）"),
                                              QStringLiteral("规范术语（多个用逗号分隔）")});
    synonymTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    synonymTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    synonymTable_->verticalHeader()->setVisible(false);
    synonymTable_->setMinimumHeight(140);
    formSyn->addRow(synonymTable_);

    auto* synButtons = new QHBoxLayout();
    synonymAddBtn_ = new QPushButton(QStringLiteral("＋ 添加映射"), this);
    synonymAddBtn_->setObjectName(QStringLiteral("settingsSynonymAdd"));
    synonymRemoveBtn_ = new QPushButton(QStringLiteral("－ 删除选中行"), this);
    synonymRemoveBtn_->setObjectName(QStringLiteral("settingsSynonymRemove"));
    synButtons->addWidget(synonymAddBtn_);
    synButtons->addWidget(synonymRemoveBtn_);
    synButtons->addStretch();
    formSyn->addRow(synButtons);
    formSyn->addRow(makeHint(this,
        QStringLiteral("检索时若查询包含某口语词，会把对应规范术语追加进 BM25 词项（扩展而非替换，")
            + QStringLiteral("原词保留）；向量语义检索不受影响。例：电信诈骗 → 诈骗,骗取。")));

    connect(synonymAddBtn_, &QPushButton::clicked,
            this, &SettingsPage::onAddSynonymRow);
    connect(synonymRemoveBtn_, &QPushButton::clicked,
            this, &SettingsPage::onRemoveSynonymRow);

    // ── 操作行 ──
    auto* actions = new QHBoxLayout();
    actions->setSpacing(10);
    defaultsBtn_ = new QPushButton(QStringLiteral("恢复默认值"), this);
    defaultsBtn_->setObjectName(QStringLiteral("settingsDefaultsBtn"));
    saveBtn_ = new QPushButton(QStringLiteral("保存并生效"), this);
    saveBtn_->setObjectName(QStringLiteral("settingsSaveBtn"));
    AppTheme::setButtonRole(saveBtn_, "primary");
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("settingsStatus"));

    actions->addWidget(defaultsBtn_);
    actions->addWidget(saveBtn_);
    actions->addStretch();
    actions->addWidget(statusLabel_);
    // 滚动区夹在页头与操作行之间（保存/恢复按钮常驻页底，不随内容滚动）
    root->addWidget(scroll, 1);
    root->addLayout(actions);

    connect(defaultsBtn_, &QPushButton::clicked,
            this, &SettingsPage::onRestoreDefaults);
    connect(saveBtn_, &QPushButton::clicked,
            this, &SettingsPage::onSave);
}

void SettingsPage::populate(const config::AppSettings& s) {
    k1Spin_->setValue(s.k1);
    bSpin_->setValue(s.b);
    bm25WeightSpin_->setValue(s.bm25Weight);
    vectorWeightSpin_->setValue(s.vectorWeight);
    topKSpin_->setValue(s.topK);
    temperatureSpin_->setValue(s.temperature);
    chunkSizeSpin_->setValue(s.chunkSize);
    chunkOverlapSpin_->setValue(s.chunkOverlap);
    chatUrlEdit_->setText(QString::fromStdString(s.chatBaseUrl));
    chatModelEdit_->setText(QString::fromStdString(s.chatModel));
    embedUrlEdit_->setText(QString::fromStdString(s.embeddingBaseUrl));
    embedModelEdit_->setText(QString::fromStdString(s.embeddingModel));
    embedKeyEdit_->setText(QString::fromStdString(s.embeddingApiKey));
}

config::AppSettings SettingsPage::valuesFromForm() const {
    config::AppSettings s;
    s.k1 = k1Spin_->value();
    s.b = bSpin_->value();
    s.bm25Weight = bm25WeightSpin_->value();
    s.vectorWeight = vectorWeightSpin_->value();
    s.topK = topKSpin_->value();
    s.temperature = temperatureSpin_->value();
    s.chunkSize = chunkSizeSpin_->value();
    // 重叠必须小于块长，否则会切出无限循环——这里直接钳住，不留静默坏配置
    s.chunkOverlap = std::min(chunkOverlapSpin_->value(), s.chunkSize - 1);
    s.chatBaseUrl = chatUrlEdit_->text().trimmed().toStdString();
    s.chatModel = chatModelEdit_->text().trimmed().toStdString();
    s.embeddingBaseUrl = embedUrlEdit_->text().trimmed().toStdString();
    s.embeddingModel = embedModelEdit_->text().trimmed().toStdString();
    s.embeddingApiKey = embedKeyEdit_->text().toStdString();
    return s;
}

void SettingsPage::onSave() {
    const config::AppSettings settings = valuesFromForm();
    std::string error;
    if (!config::AppSettings::save(config::dataFilePath(config::SETTINGS_FILE), settings, &error)) {
        statusLabel_->setText(QStringLiteral("保存失败：%1").arg(
            QString::fromStdString(error)));
        return;
    }

    // T7：同义词随「保存并生效」一并落盘 + 热更新（空表=关闭扩展，如实保存）
    const rag::synonym::SynonymMap synonyms = synonymsFromTable();
    if (!rag::synonym::save(config::dataFilePath(config::SYNONYMS_FILE), synonyms, &error)) {
        statusLabel_->setText(QStringLiteral("同义词保存失败：%1").arg(
            QString::fromStdString(error)));
        return;
    }

    statusLabel_->setText(QStringLiteral("已保存并生效（%1）").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    emit settingsChanged(settings);
    emit synonymsChanged(synonyms);
}

void SettingsPage::onRestoreDefaults() {
    populate(config::AppSettings::defaults());
    statusLabel_->setText(
        QStringLiteral("已回填默认值——点击「保存并生效」后应用"));
}

// ── T7：同义词映射表 ──

void SettingsPage::populateSynonyms(const rag::synonym::SynonymMap& map) {
    synonymTable_->setRowCount(static_cast<int>(map.entries.size()));
    for (int row = 0; row < static_cast<int>(map.entries.size()); ++row) {
        const auto& [spoken, terms] = map.entries[static_cast<size_t>(row)];
        synonymTable_->setItem(row, 0, new QTableWidgetItem(
            QString::fromStdString(spoken)));
        QStringList termList;
        for (const auto& term : terms) {
            termList << QString::fromStdString(term);
        }
        synonymTable_->setItem(row, 1, new QTableWidgetItem(termList.join(QStringLiteral(", "))));
    }
}

rag::synonym::SynonymMap SettingsPage::synonymsFromTable() const {
    rag::synonym::SynonymMap map;
    for (int row = 0; row < synonymTable_->rowCount(); ++row) {
        const auto spokenItem = synonymTable_->item(row, 0);
        const auto termItem = synonymTable_->item(row, 1);
        if (!spokenItem || !termItem) {
            continue;
        }
        const std::string spoken = spokenItem->text().trimmed().toStdString();
        if (spoken.empty()) {
            continue;
        }
        // 术语按中英文逗号切分，去空段
        QStringList rawTerms = termItem->text().split(
            QRegularExpression(QStringLiteral("[,，]")), Qt::SkipEmptyParts);
        std::vector<std::string> terms;
        for (const QString& raw : rawTerms) {
            const std::string term = raw.trimmed().toStdString();
            if (!term.empty()) {
                terms.push_back(term);
            }
        }
        if (terms.empty()) {
            continue;
        }
        // 重复口语词：后行覆盖前行（与 load 语义一致）
        bool replaced = false;
        for (auto& entry : map.entries) {
            if (entry.first == spoken) {
                entry.second = std::move(terms);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            map.entries.emplace_back(spoken, std::move(terms));
        }
    }
    return map;
}

void SettingsPage::onAddSynonymRow() {
    const int row = synonymTable_->rowCount();
    synonymTable_->insertRow(row);
    synonymTable_->setCurrentCell(row, 0);
}

void SettingsPage::onRemoveSynonymRow() {
    const int row = synonymTable_->currentRow();
    if (row >= 0) {
        synonymTable_->removeRow(row);
    }
}
