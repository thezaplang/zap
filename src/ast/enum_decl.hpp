#pragma once
#include "node.hpp"
#include "top_level.hpp"
#include "visitor.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class EnumDecl : public TopLevel {
public:
  struct Entry {
    std::string name_;
    bool hasExplicitValue_ = false;
    int64_t value_ = 0;

    Entry() = default;

    explicit Entry(std::string name)
        : name_(std::move(name)), hasExplicitValue_(false), value_(0) {}

    Entry(std::string name, int64_t value)
        : name_(std::move(name)), hasExplicitValue_(true), value_(value) {}
  };

  std::string name_;
  std::vector<Entry> entries_;

  EnumDecl() = default;

  EnumDecl(const std::string &name, std::vector<Entry> entries)
      : name_(name), entries_(std::move(entries)) {}

  // Backward-compatible constructor for existing call sites that only provide
  // names. Values are treated as implicit and can be resolved in later phases.
  EnumDecl(const std::string &name, std::vector<std::string> entryNames)
      : name_(name) {
    entries_.reserve(entryNames.size());
    for (auto &entryName : entryNames) {
      entries_.emplace_back(std::move(entryName));
    }
  }

  void accept(Visitor &v) override { v.visit(*this); }
};