import "std/prelude" { printInt };

struct Point {
    x: Int,
    y: Int
}

fun main() {
    var points: [3]Point;
    
    points[0] = Point{x: 1, y: 1};
    points[1] = Point{x: 2, y: 2};
    points[2] = Point{x: 3, y: 3};

    printInt(points[0].x);
    printInt(points[1].y);
    printInt(points[2].x);

    points[1].x = 22;
    printInt(points[1].x);

    return 0;
}
