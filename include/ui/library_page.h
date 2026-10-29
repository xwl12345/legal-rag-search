#pragma once
#include <QWidget>
#include <QString>
#include <QStringList>
#include <vector>

#include "rag/retriever.h"

class QLabel;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QLineEdit;
class QComboBox;

namespace favorite {
class FavoriteStore;
}

/// 文档库页（T1 / M1）。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本页是"插件"，只对 Retriever 做**单向只读调用**（documentInfos /
///   getDocumentInfo / getFullText / getMetadata）；Retriever 不感知本页存在。
///   引擎实例由 MainWindow 在集中注册处创建并经构造参数注入，本页不 new。
///   删除本页 = main_window.cpp 删注册几行 + 删本文件，核心检索功能零影响。
///
/// T11 起另持 FavoriteStore 指针（非拥有）：删除文档成功后把其收藏项标记
/// 「原文已删除」（快照保留可读，见 FavoriteStore::markInvalidByDocIds），
/// 不让收藏项无声悬挂成"正常"。
///
/// 职责：文档列表（名称/类型/块数/案件类型/结果倾向/导入时间）、查看元数据与
/// 全文预览、单个删除、批量删除、清空重建。
class LibraryPage : public QWidget {
    Q_OBJECT

public:
    /// @param retriever      引擎实例（非拥有；生命周期由 MainWindow 保证）
    /// @param favoriteStore  案例收藏库（T11，非拥有；可为空——空则无删除联动）
    explicit LibraryPage(rag::Retriever* retriever,
                         favorite::FavoriteStore* favoriteStore = nullptr,
                         QWidget* parent = nullptr);
    ~LibraryPage() override;

    /// 当前表格中的行数（供自动化测试断言）
    int rowCount() const;

    /// 本页是否有引擎任务进行中（删除/清空），供 closeEvent 断言
    bool isBusy() const { return busySelf_; }

public slots:
    /// 从引擎重新拉取文档列表并刷新表格与统计（供外部在导入/检索后触发）
    void refresh();

    /// 按 docId 删除单篇（供自动化测试绕过弹窗直接调用）
    /// @return 是否真的删掉了
    bool removeDocumentById(const QString& docId, bool confirm = false);

    /// 其他页面有引擎任务进行中时，禁用本页删除/清空按钮（P0-2 跨页互斥，
    /// 由 MainWindow 中转——页面之间不互相引用）
    void setExternalBusy(bool busy) {
        if (busyExternal_ != busy) {
            busyExternal_ = busy;
            refreshActionButtons();
        }
    }

    // ── T13 多知识库 ──
    /// 主窗口通知当前库已变化（启动恢复 / 切换成功后调用），刷新下拉选中项
    void setCurrentKb(const QString& relPath);
    /// 请求另存当前索引为命名库（含校验与覆盖确认；confirm=true 供测试
    /// 绕过弹窗直接覆盖）。@return 校验通过并已发出请求
    bool requestSaveKbAs(const QString& name, bool confirm = false);
    /// 请求加载指定库（relPath 空 = 默认主库）。@return 已发出请求
    bool requestSwitchKb(const QString& relPath);
    /// 请求删除指定库文件。@return 已发出请求
    bool requestDeleteKb(const QString& relPath);

signals:
    /// 文档集合发生变化（增删/清空），供主窗口刷新状态栏与检索页
    void libraryChanged();

    /// 引擎任务开始/结束（P0-2）：MainWindow 据此让其他页面禁用引擎动作
    void engineBusyChanged(bool busy);

    // ── T13 多知识库（页面不碰引擎线程，全部经 MainWindow 队列化中转）──
    /// 请求切换知识库（relPath 相对 exe 目录，空 = 默认主库）
    void kbSwitchRequested(const QString& relPath);
    /// 请求另存当前索引（relPath 形如 indexes/<名称>.dat）
    void kbSaveAsRequested(const QString& relPath);
    /// 请求删除库文件（relPath 形如 indexes/<名称>.dat；主库 MainWindow 侧拒绝）
    void kbDeleteRequested(const QString& relPath);

private slots:
    void onRemoveSelected();
    void onClearAll();
    void onSelectionChanged();
    void onRowActivated(int row, int column);
    /// T13：知识库下拉/按钮动作入口
    void onLoadKbClicked();
    void onSaveKbClicked();
    void onDeleteKbClicked();

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
    /// 按忙碌状态 + 选中情况刷新删除/清空按钮
    void refreshActionButtons();

    /// 重建表格内容
    void rebuildTable();
    /// 更新顶部统计标签
    void updateSummary();
    /// 选中集合对应的 docId 列表
    QStringList selectedDocIds() const;
    /// 把某行的完整元数据渲染到右侧详情区
    void showDetails(const QString& docId);
    /// 清空右侧详情区
    void clearDetails();

    /// T13：扫描 indexes/ 目录重建下拉项（当前项按 currentKbRel_ 选中）
    void refreshKbCombo();

    /// T11：把指定文档的收藏项标记为「原文已删除」。
    /// @return 状态翻转的收藏条数；收藏库不可用返回 -1
    int markFavoritesInvalid(const QStringList& docIds);

    rag::Retriever* retriever_ = nullptr;   // 非拥有
    favorite::FavoriteStore* favoriteStore_ = nullptr;   // T11：非拥有

    // ── P0-2 忙碌状态 ──
    bool busySelf_ = false;
    bool busyExternal_ = false;

    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* hint_ = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;

    // ── T13 知识库卡 ──
    QString currentKbRel_;          // 空 = 默认主库
    QComboBox* kbCombo_ = nullptr;
    QPushButton* kbLoadBtn_ = nullptr;
    QPushButton* kbSaveBtn_ = nullptr;
    QPushButton* kbDeleteBtn_ = nullptr;

    // 右侧详情
    QTextEdit* detailArea_ = nullptr;
    QLabel* detailTitle_ = nullptr;
};
