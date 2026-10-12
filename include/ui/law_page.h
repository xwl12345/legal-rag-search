#pragma once
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

namespace law {
class LawStore;
struct LawArticle;
}

namespace rag {
class Retriever;
}

/// 法条速查页（T9 / M7）：内置常用法条库，条号精确定位 + 关键词全文检索。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本页对 LawStore（条号查询）与独立法条 Retriever（关键词检索）都只做
///   单向只读调用；不写任何存储、不知道其它页面存在。删除本页 =
///   main_window.cpp 注册处删几行 + 删页面文件，核心检索链路零影响。
///
/// 关键词检索为何同步直调 lawRetriever（不经 EngineWorker）：
///   法条语料仅数百条且每条不足 1KB，BM25 检索毫秒级返回；独立实例与主
///   检索引擎物理隔离，不存在跨页忙碌互斥问题。实测耗时见任务卡 T9 验收。
class LawPage : public QWidget {
    Q_OBJECT
public:
    /// @param store        法条库（只读）
    /// @param lawRetriever 法条关键词检索实例（独立第二语料库，MainWindow 构建）
    explicit LawPage(law::LawStore* store, rag::Retriever* lawRetriever,
                     QWidget* parent = nullptr);

private slots:
    /// 搜索框回车 / 点「检索」：条号精确定位与关键词检索自动分流
    void onSearch();

    /// 左侧目录树选中 → 右侧详情
    void onTreeItemSelected();

    /// 关键词结果列表选中 → 右侧详情
    void onResultItemSelected();

private:
    void setupUi();
    void rebuildTree();

    /// 右侧详情区渲染单条法条（法律名 / 条号 / 正文 / 来源）
    void showArticle(const law::LawArticle& article);

    /// 恢复左侧目录模式（清空搜索框时）
    void exitResultMode();

    law::LawStore* store_ = nullptr;
    rag::Retriever* lawRetriever_ = nullptr;

    QLineEdit* queryInput_ = nullptr;
    QPushButton* searchBtn_ = nullptr;
    QLabel* statsLabel_ = nullptr;     // 收录 N 部法律 / M 条（文本来源）
    QLabel* leftTitle_ = nullptr;      // 「法律目录」/「检索结果」模式标题
    QTreeWidget* tree_ = nullptr;      // 目录模式：法律 → 条号
    QListWidget* resultList_ = nullptr;  // 结果模式：命中条文列表
    QTextBrowser* detail_ = nullptr;   // 条文详情（正文 + 来源）

    bool resultMode_ = false;
};
