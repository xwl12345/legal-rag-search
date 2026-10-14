#pragma once
#include <QString>
#include <string>
#include <vector>

namespace favorite {

/// 案件分组（律师的"类案集"容器）：名称 / 备注 / 创建时间。
/// favoriteCount 仅在 cases() 列表查询时填充（左分组列表要显示「名称（N）」）。
struct FavoriteCase {
    long long id = 0;
    std::string name;
    std::string note;
    std::string createdAt;
    int favoriteCount = 0;
};

/// 单条收藏：定位到 (docId, chunkIndex)，并**快照**收藏当时的状态。
///
/// snippet / title / score 是收藏那一刻的拷贝，不是运行期的活引用——
/// 文档被删除后索引里的块没了，靠快照仍能展示"当时收藏的内容"，
/// 并用 valid=false 标注「原文已删除」（T11 决策点：标记失效而非删项）。
/// note 是用户备注，与快照无关，重复收藏时保留不覆盖。
struct FavoriteItem {
    long long id = 0;
    long long caseId = 0;
    std::string docId;
    int chunkIndex = 0;
    std::string title;      // 收藏时的展示标题（docId + 相关度等）
    std::string snippet;    // 收藏当时的块内容快照
    std::string note;       // 用户备注
    double score = 0.0;     // 收藏时的相关度
    bool valid = true;      // false = 原文已删除（快照仍可读）
    std::string createdAt;
};

/// 案例收藏集 SQLite 存储层（T11 / M9，Qt SQL，零额外部署）。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本层属于「核心数据链路」，不得 include 任何 ui/ 头、不得持有页面指针；
///   页面对它是单向调用（收藏页管理分组与条目、检索页写入收藏、
///   文档库页删除文档后标记失效），MainWindow 是唯一持有者。
///   删除案例收藏页对存储层零影响。
///
/// 每个实例独占一个具名连接（connectionName 自增生成），析构时归还，
/// 便于单元测试里并存多个实例（分别指向不同的测试库）。
class FavoriteStore {
public:
    /// @param dbPath 库文件路径；默认 config::FAVORITE_DB（exe 所在目录）
    explicit FavoriteStore(std::string dbPath);
    FavoriteStore();
    ~FavoriteStore();

    FavoriteStore(const FavoriteStore&) = delete;
    FavoriteStore& operator=(const FavoriteStore&) = delete;

    /// 打开数据库并建表（幂等，可重复调用）。失败原因见 lastError()。
    bool open();
    bool isOpen() const { return opened_; }
    std::string lastError() const;

    // ── 案件分组 ──

    /// 新建分组。名称重名时**幂等返回已存在分组的 id**（不报错）——
    /// 收藏弹窗「选已有分组或新建」允许用户手打一个已存在的名字。
    /// 失败（含空名称）返回 0。
    long long createCase(const std::string& name, const std::string& note = "");

    /// 改分组备注；分组不存在返回 false
    bool setCaseNote(long long caseId, const std::string& note);

    /// 删除分组，连同其下全部收藏一并删除（显式事务，不依赖外键 pragma）。
    /// @return 连同删除的收藏条数；分组不存在返回 -1
    int removeCase(long long caseId);

    /// 全部分组（按创建序），favoriteCount 已填充
    std::vector<FavoriteCase> cases() const;

    /// 分组数
    int caseCount() const;

    // ── 收藏项 ──

    /// 收藏一个片段。同一 (分组, 文档, 块) 重复收藏 = **刷新快照并复活**
    /// （snippet/title/score 更新、valid 复位为 true、用户备注保留）——
    /// 文档删除后重新导入同名文档时，用户重新点一次收藏即可恢复，无需先移除。
    /// @return 收藏项 id（>0）；失败返回 0
    long long addFavorite(const FavoriteItem& item);

    /// 移除单条收藏；不存在返回 false（幂等）
    bool removeFavorite(long long id);

    /// 改收藏备注；不存在返回 false
    bool setFavoriteNote(long long id, const std::string& note);

    /// 某分组的全部收藏（按收藏时间倒序，新在上）
    std::vector<FavoriteItem> favoritesOf(long long caseId) const;

    /// 全部收藏（按分组 + 收藏时间倒序；供测试与整库对账）
    std::vector<FavoriteItem> allFavorites() const;

    /// 某分组的收藏条数
    int favoriteCountOf(long long caseId) const;

    // ── 文档删除联动（T11 验收第二条）──

    /// 按 docId 把收藏项标记为「原文已删除」（valid=0，快照保留可读）。
    /// @return 本次**状态发生翻转**的行数（已失效的重复标记不计）——
    ///          文档库页据此提示"已将 N 条收藏标记失效"。
    int markInvalidByDocIds(const std::vector<std::string>& docIds);

    const std::string& dbPath() const { return dbPath_; }

private:
    std::string dbPath_;
    QString connectionName_;
    bool opened_ = false;
    std::string lastError_;
};

}  // namespace favorite
