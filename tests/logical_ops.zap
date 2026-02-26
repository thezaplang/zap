fun main() Int {
    var a: Bool = true;
    var b: Bool = false;

    if a && b {
        println("fail 1");
    } else {
        println("pass 1a");
    }

    if a || b {
        println("pass 1b");
    } else {
        println("fail 1b");
    }

    if false && (1 / 0 == 0) {
        println("fail 2");
    } else {
        println("pass 2 (&& short-circuit)");
    }

    if true || (1 / 0 == 0) {
        println("pass 3 (|| short-circuit)");
    } else {
        println("fail 3");
    }

    return 0;
}
