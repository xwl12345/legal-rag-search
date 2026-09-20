#include "index/inverted_index.h"
#include <algorithm>
#include <cmath>

namespace search_index {

void InvertedIndex::addDocument(const std::string& docId,
                                 int chunkIndex,
                                 const std::vector<std::string>& terms) {
    // 计算词频
    std::unordered_map<std::string, int> tf;
    for (const auto& t : terms) {
        tf[t]++;
    }

    // 写入倒排索引
    std::string docKey = docId + ":" + std::to_string(chunkIndex);
    for (const auto& [term, freq] : tf) {
        Posting posting;
        posting.docId = docId;
        posting.chunkIndex = chunkIndex;
        posting.termFreq = freq;
        index_[term].push_back(posting);
    }

    // 记录文档长度
    docLengths_[docKey] = static_cast<int>(terms.size());
    totalDocs_++;
}

const std::vector<Posting>* InvertedIndex::getPostings(const std::string& term) const {
    auto it = index_.find(term);
    if (it != index_.end()) {
        return &it->second;
    }
    return nullptr;
}

int InvertedIndex::docLength(const std::string& docId, int chunkIndex) const {
    std::string key = docId + ":" + std::to_string(chunkIndex);
    auto it = docLengths_.find(key);
    return (it != docLengths_.end()) ? it->second : 0;
}

int InvertedIndex::docFreq(const std::string& term) const {
    auto it = index_.find(term);
    return (it != index_.end()) ? static_cast<int>(it->second.size()) : 0;
}

std::vector<std::pair<std::string, int>> InvertedIndex::allDocs() const {
    std::vector<std::pair<std::string, int>> result;
    for (const auto& [key, _] : docLengths_) {
        auto colonPos = key.rfind(':');
        if (colonPos != std::string::npos) {
            std::string docId = key.substr(0, colonPos);
            int chunkIndex = std::stoi(key.substr(colonPos + 1));
            result.emplace_back(docId, chunkIndex);
        }
    }
    return result;
}

double InvertedIndex::avgDocLength() const {
    if (docLengths_.empty()) return 0.0;
    long long total = 0;
    for (const auto& [_, len] : docLengths_) {
        total += len;
    }
    return static_cast<double>(total) / docLengths_.size();
}

int InvertedIndex::removeChunk(const std::string& docId, int chunkIndex) {
    const std::string docKey = docId + ":" + std::to_string(chunkIndex);

    // 块长度记录：不存在说明该块本就没被索引，直接返回 0（幂等）
    auto lenIt = docLengths_.find(docKey);
    if (lenIt == docLengths_.end()) {
        return 0;
    }
    docLengths_.erase(lenIt);
    totalDocs_--;

    // 剔除各词项倒排表中属于该块的 posting；词项清空后删除词项
    int removed = 0;
    for (auto it = index_.begin(); it != index_.end(); ) {
        auto& postings = it->second;
        const size_t before = postings.size();
        postings.erase(
            std::remove_if(postings.begin(), postings.end(),
                           [&](const Posting& p) {
                               return p.docId == docId &&
                                      p.chunkIndex == chunkIndex;
                           }),
            postings.end());
        removed += static_cast<int>(before - postings.size());

        if (postings.empty()) {
            it = index_.erase(it);
        } else {
            ++it;
        }
    }

    return removed;
}

void InvertedIndex::clear() {
    index_.clear();
    docLengths_.clear();
    totalDocs_ = 0;
}

} // namespace index
