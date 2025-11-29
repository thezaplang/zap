//
// Created by Funcieq on 27.11.2025.
//

// Simple arena allocator for AST Nodes.

#ifndef IGNIS_ARENA_H
#define IGNIS_ARENA_H

#include "node.h"
#include <vector>
#include <stdexcept>

class NodeArena
{
public:
    NodeArena() = default;

    explicit NodeArena(size_t reserve_count)
    {
        nodes.reserve(reserve_count);
    }

    // Returns its NodeId.
    NodeId create(Node n)
    {
        nodes.push_back(std::move(n));
        return static_cast<NodeId>(nodes.size() - 1);
    }

    // Access
    Node &get(NodeId id)
    {
        if (id >= nodes.size())
            throw std::out_of_range("NodeArena: invalid NodeId");
        return nodes[id];
    }

    const Node &get(NodeId id) const
    {
        if (id >= nodes.size())
            throw std::out_of_range("NodeArena: invalid NodeId");
        return nodes[id];
    }

    void reserve(size_t n) { nodes.reserve(n); }
    size_t size() const { return nodes.size(); }
    std::vector<Node> &getAllNodes() { return nodes; }

private:
    std::vector<Node> nodes;
};

#endif // IGNIS_ARENA_H
