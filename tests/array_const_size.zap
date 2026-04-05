import "std/prelude" { printInt };

const NUM: Int = 3;

fun main() Int {
    var xs: [NUM]Int = { 1, 2, 3 };
    printInt(xs[0]);
    printInt(xs[1]);
    printInt(xs[2]);
    return 0;
}
