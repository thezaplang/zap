#pragma once
#include <cstdint>

struct SourceSpan {
  size_t start = 0;
  size_t end = 0;
};

class Node {
public:
  SourceSpan span;
  virtual ~Node() = default;
};
