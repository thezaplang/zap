fun classify(n: Int) Int {
    return n > 0 ? 1 : 2;
}

fun nested(a: Bool, b: Bool) Int {
    return a ? (b ? 10 : 20) : 30;
}

fun short_circuit(flag: Bool) Int {
    return flag ? 7 : (1 / 0);
}

fun main() Int {
    var x: Int = true ? 5 : 9;
    var y: Int = false ? 1 : 2 + 3;

    if x != 5 { return 1; }
    if y != 5 { return 2; }
    if classify(4) != 1 { return 3; }
    if classify(-1) != 2 { return 4; }
    if nested(true, false) != 20 { return 5; }
    if nested(false, true) != 30 { return 6; }
    if short_circuit(true) != 7 { return 7; }

    return 0;
}
