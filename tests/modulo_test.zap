fun main() {
    var cap: UInt64 = 256;
    var i: UInt64 = 150;
    while i < 160 {
        printInt(i % cap);
        i = i + 1;
    }

    var a: Int = -10;
    var b: Int = 3;
    printInt(a % b);

    var f1: Float = 10.5;
    var f2: Float = 3.0;
    printFloat(f1 % f2);
}
