#pragma once
#include <cstddef>
#include <cstdint>

#include "../token/token.hpp"

struct Visitor;

class Node {
public:
    SourceSpan span;
    virtual ~Node() noexcept = default;
    virtual void accept(Visitor& v) = 0;
};
