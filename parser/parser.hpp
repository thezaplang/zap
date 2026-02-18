#pragma once
#include "../token/token.hpp"
#include <vector>
namespace zap{

    class Parser{
      public:
        Parser();
        ~Parser();
        void parse(const std::vector<Token>& toks);
      private:
    };
}
