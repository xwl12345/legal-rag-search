#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace search_index {

/// 倒排索引条目
struct Posting {
    std::string docId;
    int chunkIndex = 0;
    int termFreq = 0;          // 该词在此文档中的词频
};

/// 倒排索引：词 → 文档列表映射
class InvertedIndex {
public:
    /// 为一个文档块建索引
    void addDocument(const std::string& docId,
                     int chunkIndex,
                     const std::vector<std::string>& terms);

    /// 查询词对应的倒排列表
    const std::vector<Posting>* getPostings(const std::string& term) const;

    /// 获取文档总数
    int totalDocs() const { return totalDocs_; }

    /// 获取文档长度（词数）
    int docLength(const std::string& docId, int chunkIndex) const;

    /// 获取词项对应的文档频率
    int docFreq(const std::string& term) const;

    /// 获取所有文档 ID 列表
    std::vector<std::pair<std::string, int>> allDocs() const;

    /// 获取平均文档长度
    double avgDocLength() const;

    /// 清空索引
    void clear();

    /// 索引大小
    size_t size() const { return index_.size(); }

private:
    // term → postings list
    std::unordered_map<std::string, std::vector<Posting>> index_;

    // (docId, chunkIndex) → term count
    std::unordered_map<std::string, int> docLengths_;

    int totalDocs_ = 0;
};

} // namespace index
