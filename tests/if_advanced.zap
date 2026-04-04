fun test_nested(a: Bool, b: Bool) Int {
    return if a {
        if b { 1 } else { 2 }
    } else {
        3
    };
}

fun test_math(a: Bool) Int {
    var x: Int = 10 + (if a { 5 } else { 1 });
    return x;
}

fun test_complex_cond(a: Int, b: Int) Int {
    return if (if a > b { true } else { false }) {
        a
    } else {
        b
    };
}

fun main() Int {
    if test_nested(true, true) != 1 { return 1; }
    if test_nested(true, false) != 2 { return 2; }
    if test_nested(false, true) != 3 { return 3; }
    
    if test_math(true) != 15 { return 4; }
    if test_math(false) != 11 { return 5; }
    
    if test_complex_cond(10, 5) != 10 { return 6; }
    if test_complex_cond(5, 10) != 10 { return 7; }
    
    printInt(100); // Success marker
    return 0;
}
