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

/// 文档库页（T1 / M1）。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本页是"插件"，只对 Retriever 做**单向只读调用**（documentInfos /
///   getDocumentInfo / getFullText / getMetadata）；Retriever 不感知本页存在。
///   引擎实例由 MainWindow 在集中注册处创建并经构造参数注入，本页不 new。
///   删除本页 = main_window.cpp 删注册几行 + 删本文件，核心检索功能零影响。
///
/// 职责：文档列表（名称/类型/块数/案件类型/结果倾向/导入时间）、查看元数据与
/// 全文预览、单个删除、批量删除、清空重建。
class LibraryPage : public QWidget {
    Q_OBJECT

public:
    /// @param retriever 引擎实例（非拥有；生命周期由 MainWindow 保证）
    explicit LibraryPage(rag::Retriever* retriever, QWidget* parent = nullptr);
    ~LibraryPage() override;

    /// 当前表格中的行数（供自动化测试断言）
    int rowCount() const;

public slots:
    /// 从引擎重新拉取文档列表并刷新表格与统计（供外部在导入/检索后触发）
    void refresh();

    /// 按 docId 删除单篇（供自动化测试绕过弹窗直接调用）
    /// @return 是否真的删掉了
    bool removeDocumentById(const QString& docId, bool confirm = false);

signals:
    /// 文档集合发生变化（增删/清空），供主窗口刷新状态栏与检索页
    void libraryChanged();

private slots:
    void onRemoveSelected();
    void onClearAll();
    void onSelectionChanged();
    void onRowActivated(int row, int column);

private:
    void setupUi();
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

    rag::Retriever* retriever_ = nullptr;   // 非拥有

    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* hint_ = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;

    // 右侧详情
    QTextEdit* detailArea_ = nullptr;
    QLabel* detailTitle_ = nullptr;
};
