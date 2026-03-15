struct Point {
    x: Int,
    y: Int
}

fun createPoint(x: Int, y: Int) Point {
    return Point{x: x, y: y};
}

fun sumPoint(p: Point) Int {
    return p.x + p.y;
}

fun movePoint(p: Point, dx: Int, dy: Int) Point {
    p.x = p.x + dx;
    p.y = p.y + dy;
    return p;
}

fun main() {
    var p1: Point = createPoint(10, 20);
    printInt(p1.x); // Expected: 10
    printInt(p1.y); // Expected: 20

    var s: Int = sumPoint(p1);
    printInt(s); // Expected: 30

    var p2: Point = movePoint(p1, 5, 5);
    printInt(p2.x); // Expected: 15
    printInt(p2.y); // Expected: 25
    
    // Check if p1 was modified (pass by value or reference?)
    // In many languages, structs are value types.
    printInt(p1.x); 

    return 0;
}
