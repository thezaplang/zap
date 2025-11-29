#include <iostream>

#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/sema/sema.h"
#define VERSION "pre alpha 0.1"

#include <fstream>
#include <sstream>
#include <cstring>

void help()
{
    std::cout << "Ignis V" << VERSION << std::endl;
    std::cout << "      -flag [<value>]" << std::endl;
    std::cout << "\n";
    std::cout << "      -c [<path>]" << std::endl;
    std::cout << "          compiles provied files\n";
    std::cout << "      -v\n";
    std::cout << "          displays installed version of Ignis\n";
    std::cout << "      -h\n";
    std::cout << "        displays this help\n";
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        help();
        return 0;
    }

    // CLI: -h, -v, -c <path>
    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "-help") == 0)
        {
            help();
            return 0;
        }
        if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "-version") == 0)
        {
            std::cout << VERSION << std::endl;
            return 0;
        }
        if (std::strcmp(a, "-c") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: -c requires a path argument\n";
                return 1;
            }
            const char *path = argv[++i];
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in)
            {
                std::cerr << "Failed to open file: " << path << std::endl;
                return 2;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string content = ss.str();

            Lexer lexer;
            auto toks = lexer.Tokenize(content);

            for (auto &tok : toks)
            {
                std::cout << "type= " << tok.type << " value= " << tok.value << std::endl;
            }

            Parser parser;
            parser.Parse(&toks, content, path);
            SemanticAnalyzer sema;
            sema.Analyze(parser.arena, content, path);

            if (sema.foundMain == false)
            {
                std::cerr << "Error: 'main' function not found\n";
                return 4;
            }

            if (!parser.errors.empty())
            {
                std::cerr << "Parsing finished with " << parser.errors.size() << " error(s)\n";
                return 3;
            }

            return 0;
        }
    }

    help();
    return 0;
}