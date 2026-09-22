#include "ui/settings_page.h"

#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "config/app_config.h"
#include "ui/app_theme.h"

namespace {

/// 给按钮打 role 属性，配色交给全局 QSS 的属性选择器（与 history_page 同款）
void setButtonRole(QPushButton* button, const char* role) {
    button->setProperty("role", QString::fromUtf8(role));
}

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

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent) {
    setupUi();

    // 启动时读一次配置文件；读不到（首次运行 / 文件损坏）就填默认值，
    // 与 AppSettings::load 的容错约定一致——绝不让表单空着或读到 0。
    config::AppSettings settings;
    config::AppSettings::load(config::SETTINGS_FILE, settings);
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

    // ── 卡片一：检索参数 ──
    QFormLayout* form1 = nullptr;
    makeCard(this, root, QStringLiteral("检索参数（保存后立即生效，无需重建索引）"), form1);

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
    makeCard(this, root, QStringLiteral("分块参数（仅对之后导入的文档生效）"), form2);

    chunkSizeSpin_ = makeIntSpin(this, 64, 4096);
    chunkSizeSpin_->setObjectName(QStringLiteral("settingsChunkSize"));
    chunkOverlapSpin_ = makeIntSpin(this, 0, 1024);
    chunkOverlapSpin_->setObjectName(QStringLiteral("settingsChunkOverlap"));

    form2->addRow(QStringLiteral("块最大字符数"), chunkSizeSpin_);
    form2->addRow(QStringLiteral("相邻块重叠字符数"), chunkOverlapSpin_);
    form2->addRow(makeHint(this,
        QStringLiteral("已导入文档的块边界在导入时已固定：改小参数不会把现有文本块重新切，")
            + QStringLiteral("只影响之后导入的文档；需要全库生效请清空索引后重新导入。")));

    // ── 卡片三：Embedding 服务 ──
    QFormLayout* form3 = nullptr;
    makeCard(this, root, QStringLiteral("Embedding 服务（检索质量分析 T4 将消费）"), form3);

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

    // ── 操作行 ──
    auto* actions = new QHBoxLayout();
    actions->setSpacing(10);
    defaultsBtn_ = new QPushButton(QStringLiteral("恢复默认值"), this);
    defaultsBtn_->setObjectName(QStringLiteral("settingsDefaultsBtn"));
    saveBtn_ = new QPushButton(QStringLiteral("保存并生效"), this);
    saveBtn_->setObjectName(QStringLiteral("settingsSaveBtn"));
    setButtonRole(saveBtn_, "primary");
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("settingsStatus"));

    actions->addWidget(defaultsBtn_);
    actions->addWidget(saveBtn_);
    actions->addStretch();
    actions->addWidget(statusLabel_);
    root->addLayout(actions);

    root->addStretch();

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
    s.embeddingBaseUrl = embedUrlEdit_->text().trimmed().toStdString();
    s.embeddingModel = embedModelEdit_->text().trimmed().toStdString();
    s.embeddingApiKey = embedKeyEdit_->text().toStdString();
    return s;
}

void SettingsPage::onSave() {
    const config::AppSettings settings = valuesFromForm();
    std::string error;
    if (!config::AppSettings::save(config::SETTINGS_FILE, settings, &error)) {
        statusLabel_->setText(QStringLiteral("保存失败：%1").arg(
            QString::fromStdString(error)));
        return;
    }
    statusLabel_->setText(QStringLiteral("已保存并生效（%1）").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    emit settingsChanged(settings);
}

void SettingsPage::onRestoreDefaults() {
    populate(config::AppSettings::defaults());
    statusLabel_->setText(
        QStringLiteral("已回填默认值——点击「保存并生效」后应用"));
}
