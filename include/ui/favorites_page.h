#pragma once
#include <QWidget>

#include <string>

class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTextBrowser;

namespace favorite {
class FavoriteStore;
struct FavoriteItem;
}

/// 案例收藏页（T11 / M9）：律师攒类案集的工作台。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本页是"插件"，只对 FavoriteStore 做单向调用（分组与收藏的增删改查）；
///   FavoriteStore 不感知本页存在。删除本页 = main_window.cpp 删注册几行 +
///   删页面文件，核心检索链路零影响。
///   本页**不持有 SearchPage**：收藏由检索问答页写入（经 MainWindow 的
///   favoriteChanged 信号中转刷新本页），两页互不认识。
///
/// 布局：左侧案件分组列表（新建 / 备注 / 删除），右侧收藏条目表格
///（备注 / 移除 / 打开全文——T10 全文阅读页未完成，入口置灰预留）。
/// 失效收藏（文档已删除）保留快照可读，状态列标「原文已删除」。
class FavoritesPage : public QWidget {
    Q_OBJECT

public:
    /// @param store 收藏存储层（非拥有；生命周期由 MainWindow 保证）
    explicit FavoritesPage(favorite::FavoriteStore* store, QWidget* parent = nullptr);
    ~FavoritesPage() override;

    /// 左侧分组数（供自动化测试断言）
    int caseCount() const;

    /// 当前选中分组的收藏条数（右侧表格行数；无选中分组为 0）
    int currentFavoriteCount() const;

    /// 当前选中分组名（无选中为空串）
    QString currentCaseName() const;

    /// 右侧表格第 row 行第 col 列的文本（供自动化测试断言状态列等内容）
    QString cellText(int row, int col) const;

public slots:
    /// 从存储层重新拉取分组与收藏并刷新界面（收藏写入/删除文档后由
    /// MainWindow 中转调用；手动「刷新」按钮也走这里）
    void refresh();

    /// 按主键移除单条收藏（供自动化测试绕过弹窗直接调用）
    /// @return 是否真的删掉了
    bool removeFavoriteById(long long id, bool confirm = false);

private slots:
    void onNewCase();
    void onEditCaseNote();
    void onRemoveCase();
    void onCaseSelectionChanged();
    void onFavoriteSelectionChanged();
    void onEditFavoriteNote();
    void onRemoveFavorite();
    void onOpenFullText();

private:
    void setupUi();
    void rebuildCaseList();
    void rebuildFavoriteTable();
    void updateSummary();
    void showUnavailable(const QString& reason);
    void showFavoriteDetail(const favorite::FavoriteItem& item);
    void clearDetail();
    /// 左侧选中分组的 id（无选中返回 0）
    long long currentCaseId() const;
    /// 右侧选中收藏的 id（无选中返回 0）
    long long selectedFavoriteId() const;
    /// 按分组名查 id（不存在返回 0）
    long long caseIdByName(const QString& name) const;

    favorite::FavoriteStore* store_ = nullptr;   // 非拥有

    QLabel* summary_ = nullptr;
    QLabel* hint_ = nullptr;
    QListWidget* caseList_ = nullptr;
    QTableWidget* favoriteTable_ = nullptr;
    QTextBrowser* detail_ = nullptr;

    QPushButton* newCaseBtn_ = nullptr;
    QPushButton* caseNoteBtn_ = nullptr;
    QPushButton* removeCaseBtn_ = nullptr;
    QPushButton* favoriteNoteBtn_ = nullptr;
    QPushButton* removeFavoriteBtn_ = nullptr;
    QPushButton* openFullTextBtn_ = nullptr;
};
