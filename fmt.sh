find ./ -iname "*.cpp" -o -iname "*.hpp" | xargs clang-format -i
find ./ -iname "*.c" -o -iname "*.h" | xargs clang-format -i