#pragma once
#include "node.hpp"
#include "visitor.hpp"
#include "../visibility.hpp"

class TopLevel : public virtual Node {
public:
  Visibility visibility_ = Visibility::Private;

  virtual ~TopLevel() noexcept = default;

  void accept(Visitor &v) override { v.visit(*this); }
};
