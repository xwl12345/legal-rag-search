#ifndef LIMONP_LOCAL_VECTOR_HPP
#define LIMONP_LOCAL_VECTOR_HPP

#include <iostream>
#include <vector>

namespace limonp {
using namespace std;

template <class T>
class LocalVector {
 public:
  typedef const T* const_iterator;
  typedef T value_type;
  typedef size_t size_type;

  LocalVector() {}
  LocalVector(const_iterator begin, const_iterator end) : data_(begin, end) {}
  LocalVector(size_t size, const T& t) : data_(size, t) {}

  T& operator[](size_t i) {
    return data_[i];
  }
  const T& operator[](size_t i) const {
    return data_[i];
  }

  void push_back(const T& value) {
    data_.push_back(value);
  }
  void reserve(size_t size) {
    data_.reserve(size);
  }
  bool empty() const {
    return data_.empty();
  }
  size_t size() const {
    return data_.size();
  }
  size_t capacity() const {
    return data_.capacity();
  }
  const_iterator begin() const {
    return data_.data();
  }
  const_iterator end() const {
    return data_.data() + data_.size();
  }
  void clear() {
    data_.clear();
  }

  bool operator==(const LocalVector<T>& rhs) const {
    return data_ == rhs.data_;
  }
  bool operator!=(const LocalVector<T>& rhs) const {
    return !(*this == rhs);
  }

 private:
  vector<T> data_;
};

template <class T>
ostream & operator << (ostream& os, const LocalVector<T>& vec) {
  if(vec.empty()) {
    return os << "[]";
  }
  os<<"[\""<<vec[0];
  for(size_t i = 1; i < vec.size(); i++) {
    os<<"\", \""<<vec[i];
  }
  os<<"\"]";
  return os;
}

}

#endif
