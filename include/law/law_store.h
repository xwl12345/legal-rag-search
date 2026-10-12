#pragma once
#include <QString>
#include <string>
#include <vector>

namespace law {

/// 一部法律的元信息（T9 法条速查）
struct LawInfo {
    std::string name;        // 法律名称，如「中华人民共和国劳动法」
    std::string amendment;   // 通过/修正沿革（公布文本说明）
    std::string sourceDesc;  // 文本来源说明（官方发布渠道）
    std::string sourceUrl;   // 来源页 URL
    std::string fetchedAt;   // 抓取日期
};

/// 单条法条（正文 + 来源，随查随带，页面直接展示）
struct LawArticle {
    std::string lawName;     // 法律名称
    std::string articleNo;   // 条号，如「第一千零七十九条」
    std::string text;        // 条文正文（官方发布文本原文）
    std::string sourceDesc;  // 文本来源说明
    std::string sourceUrl;   // 来源页 URL
};

/// 法条库 SQLite 存储层（Qt SQL，零额外部署 —— SQLite 驱动随 Qt 分发）。
///
/// 数据来源（T9 第一红线）：law_corpus.db 由 tools/build_law_db.py 从
/// data/law_corpus/*.json 构建，JSON 由 tools/parse_law_html.py 从官方
/// 发布页原始快照逐条解析而来；本层只读，不提供任何写接口 —— 法条
/// 语料的变更必须走「官方文本 → JSON → 重建库」链路，严禁运行期改写。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本层属于「核心数据链路」，不得 include 任何 ui/ 头、不得持有页面指针；
///   页面对它是单向调用。法条关键词检索由独立的 rag::Retriever 实例承担
///   （MainWindow 创建并持有），与本层互不依赖。
///
/// 每个实例独占一个具名连接（connectionName 自增生成），析构时归还，
/// 便于单元测试里并存多个实例（分别指向不同的测试库）。
class LawStore {
public:
    /// @param dbPath 库文件路径；默认 config::LAW_DB（exe 所在目录）
    explicit LawStore(std::string dbPath);
    LawStore();
    ~LawStore();

    LawStore(const LawStore&) = delete;
    LawStore& operator=(const LawStore&) = delete;

    /// 打开数据库（只读语义）。失败原因见 lastError()。
    bool open();
    bool isOpen() const { return opened_; }
    std::string lastError() const;

    /// 收录的法律列表（按 id 序，即 JSON 文件名字典序）
    std::vector<LawInfo> laws() const;

    /// 收录的法律部数
    int lawCount() const;

    /// 收录的条文总数
    int articleCount() const;

    /// 条号精确定位（法律名称 + 条号）；未命中返回 false
    bool findByArticle(const std::string& lawName, const std::string& articleNo,
                       LawArticle& out) const;

    /// 跨法律条号定位（仅条号；同名条号可能命中多条）
    std::vector<LawArticle> findByArticleNo(const std::string& articleNo) const;

    /// 指定法律的全部条文（按条号序；供构建关键词检索实例）
    std::vector<LawArticle> articlesOf(const std::string& lawName) const;

    /// 全部条文（按法律 + 条号序；供构建关键词检索实例）
    std::vector<LawArticle> allArticles() const;

    const std::string& dbPath() const { return dbPath_; }

private:
    std::string dbPath_;
    QString connectionName_;
    bool opened_ = false;
};

}  // namespace law
