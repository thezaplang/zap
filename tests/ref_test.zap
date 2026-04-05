import "std/prelude" { println };

fun increment(ref x: Int) {
    x = x + 1;
}

fun swap(ref x: Int, ref y: Int) {
    var tmp: Int = x;
    x = y;
    y = tmp;
}

fun main() Int {
    var a: Int = 10;
    increment(ref a);
    if (a == 11) {
        println("Ref test passed!");
    } else {
        println("Ref test failed!");
        return 1;
    }
    
    var b: Int = 20;
    var c: Int = 30;
    swap(ref b, ref c);
    if (b == 30 && c == 20) {
        println("Swap ref test passed!");
    } else {
        println("Swap ref test failed!");
        return 1;
    }
    
    return 0;
}
