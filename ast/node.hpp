#pragma once
#include <cstdint>

struct SourceSpan {
  unsigned int start = 0;
  unsigned int end = 0;
};

class Node {
public:
  SourceSpan span;
  virtual ~Node() = default;
};
