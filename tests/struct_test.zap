import "std/prelude" { printInt };

struct Point {
    x: Int,
    y: Int
}

fun main() {
    var p: Point = Point{x: 10, y: 20};
    printInt(p.x);
    printInt(p.y);
    
    p.x = 30;
    printInt(p.x);
    
    return 0;
}
