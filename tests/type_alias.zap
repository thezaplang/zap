alias MyInt = Int;

struct Point {
  x: Int,
  y: Int
}

alias MyPoint = Point;

fun main() MyInt {
  var x: MyInt = 42;
  var p: MyPoint = MyPoint { x: x, y: 10 };
  ret p.x;
}
