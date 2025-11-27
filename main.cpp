#include <iostream>

#include "frontend/lexer/lexer.h"
#define VERSION "pre alpha 0.1"

void help() {
    std::cout << "Ignis V" <<VERSION << std::endl;
    std::cout << "      -flag [<value>]"<< std::endl;
    std::cout << "\n";
    std::cout << "      -c [<path>]"<< std::endl;
    std::cout << "          compiles provied files\n";
    std::cout << "      -v\n";
    std::cout << "          displays installed version of Ignis\n";
    std::cout << "      -h\n";
    std::cout << "        displays this help\n";
}

int main(int argc, char* argv[]) {
    //ignis -c file.ign fil1.ign
    std::string_view inp("fn main(){"
                         "let a: i32 = 5*5;"
                         "return 0;"
                         "}");

    Lexer lexer;
    auto toks = lexer.Tokenize(inp);

    for (auto& tok : toks) {
        std::cout<<"type= "<<tok.type<<" value= "<<tok.value<<std::endl;
    }
    // if (argc < 2) {
    //     help();
    // }else {
    //     std::string command = argv[1];
    //     if (command == "-h" || command == "-help") {
    //         help();
    //     }else if (command == "-v" || command == "-version") {
    //         std::cout << VERSION << std::endl;
    //     }else {
    //
    //     }
    // }
    return 0;
}