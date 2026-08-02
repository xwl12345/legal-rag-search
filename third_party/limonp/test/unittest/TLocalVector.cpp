#include "limonp/LocalVector.hpp"
#include "limonp/StdExtension.hpp"
#include <fstream>
#include "gtest/gtest.h"

using namespace limonp;

TEST(LocalVector, test1) {
  LocalVector<size_t> vec;
  ASSERT_EQ(vec.size(), 0u);
  ASSERT_EQ(vec.capacity(), 0u);
  ASSERT_TRUE(vec.empty());
  size_t size = 129;
  for(size_t i = 0; i < size; i++) {
    vec.push_back(i);
  }
  ASSERT_EQ(vec.size(), size);
  ASSERT_GE(vec.capacity(), size);
  ASSERT_FALSE(vec.empty());
  const size_t* begin = vec.begin();
  ASSERT_EQ(begin[0], 0u);
  LocalVector<size_t> vec2(vec);
  ASSERT_EQ(vec2.size(), vec.size());
  ASSERT_EQ(vec2, vec);
}

TEST(LocalVector, test2) {
  LocalVector<size_t> vec;
  ASSERT_EQ(vec.size(), 0u);
  ASSERT_EQ(vec.capacity(), 0u);
  ASSERT_TRUE(vec.empty());
  size_t size = 1;
  for(size_t i = 0; i < size; i++) {
    vec.push_back(i);
  }
  ASSERT_EQ(vec.size(), size);
  ASSERT_GE(vec.capacity(), size);
  ASSERT_FALSE(vec.empty());
  LocalVector<size_t> vec2;
  vec2 = vec;
  ASSERT_EQ(vec2.size(), vec.size());
  ASSERT_EQ(vec2, vec);
}

TEST(LocalVector, NonTrivialValue) {
  LocalVector<std::string> vec;
  vec.push_back("alpha");
  vec.push_back("beta");
  LocalVector<std::string> vec2(vec);
  ASSERT_EQ(vec2, vec);

  LocalVector<std::string> vec3(vec.begin(), vec.end());
  ASSERT_EQ(vec3, vec);
}
