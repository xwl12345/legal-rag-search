#pragma once
#include <QWidget>

#include "config/app_settings.h"

class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QPushButton;

namespace config {
class AppSettings;
}

/// 设置页（T3 检索参数配置中心）：参数表单 + 保存并生效 + 恢复默认。
///
/// 解耦约定（全局约束第 8 条）：本页**只读写配置层**，不持有、不直接调用
/// Retriever——保存成功后发 settingsChanged(AppSettings)，由 MainWindow
/// 中转给引擎（热更新 setter）和需要的页面。删掉本页 = 去掉注册几行 +
/// 删页面文件，检索功能零影响。
class SettingsPage : public QWidget {
    Q_OBJECT

public:
    /// @param settings 启动配置（P3：由 MainWindow 注入，本页不再自读 JSON 文件——
    ///                 消灭同一文件双 load 的状态分叉隐患）
    explicit SettingsPage(const config::AppSettings& settings, QWidget* parent = nullptr);

    /// 把一组配置回填进表单（启动时 / 恢复默认后）
    void populate(const config::AppSettings& settings);

signals:
    /// 用户点击「保存并生效」且写盘成功后发出；MainWindow 负责转发给引擎与各页
    void settingsChanged(const config::AppSettings& settings);

private slots:
    void onSave();
    void onRestoreDefaults();

private:
    void setupUi();
    /// 从表单收集当前值（控件范围已兜底非法输入；overlap 会钳到 < chunkSize）
    config::AppSettings valuesFromForm() const;

    // ── 检索参数（查询期，保存后立即生效）──
    QDoubleSpinBox* k1Spin_ = nullptr;
    QDoubleSpinBox* bSpin_ = nullptr;
    QDoubleSpinBox* bm25WeightSpin_ = nullptr;
    QDoubleSpinBox* vectorWeightSpin_ = nullptr;
    QSpinBox* topKSpin_ = nullptr;
    QDoubleSpinBox* temperatureSpin_ = nullptr;

    // ── 分块参数（仅对之后导入的文档生效）──
    QSpinBox* chunkSizeSpin_ = nullptr;
    QSpinBox* chunkOverlapSpin_ = nullptr;

    // ── 生成（LLM）服务（P3：与 Embedding 卡片并列；Key 归检索页管）──
    QLineEdit* chatUrlEdit_ = nullptr;
    QLineEdit* chatModelEdit_ = nullptr;

    // ── Embedding 服务（T4 消费预留）──
    QLineEdit* embedUrlEdit_ = nullptr;
    QLineEdit* embedModelEdit_ = nullptr;
    QLineEdit* embedKeyEdit_ = nullptr;

    QPushButton* saveBtn_ = nullptr;
    QPushButton* defaultsBtn_ = nullptr;
    QLabel* statusLabel_ = nullptr;   // 保存结果反馈（成功时间 / 失败原因）
};
