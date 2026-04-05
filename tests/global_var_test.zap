import "std/prelude" { printInt };

global var g: Int = 42;

fun main() {
  printInt(g);
  g = 100;
  printInt(g);
}
