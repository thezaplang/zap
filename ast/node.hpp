#pragma once
#include <cstdint>
#include <cstddef>

struct SourceSpan {
  size_t start = 0;
  size_t end = 0;
};

class Visitor;

class Node {
public:
  SourceSpan span;
  virtual ~Node() = default;
  virtual void accept(Visitor &v) = 0;
};
