#pragma once
#include <cstddef>
#include <cstdint>

#include "../token/token.hpp"

struct Visitor;

class Node {
public:
  SourceSpan span;
  virtual ~Node() = default;
  virtual void accept(Visitor &v) = 0;
};
