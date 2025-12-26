#include "node.hpp"
#include <vector>
#include <memory>
#include <iostream>
class RootNode : public Node {
public:
    std::vector<std::unique_ptr<Node>> children;
    RootNode() = default;
    ~RootNode() override = default;
    void addChild(std::unique_ptr<Node> child) {
        if(child){
            children.push_back(std::move(child));
        } else {
            std::cerr<<"Cannot add a null child to RootNode"<<std::endl;
        }
    }
};