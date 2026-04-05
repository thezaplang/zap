fun classify(n: Int) Int {
    if n < 0 {
        return 1;
    } else if n == 0 {
        return 2;
    } else if n < 10 {
        return 3;
    } else {
        return 4;
    }

    return 5;
}

fun main() Int {
    if classify(-5) != 1 {
        return 1;
    }
    if classify(0) != 2 {
        return 2;
    }
    if classify(7) != 3 {
        return 3;
    }
    if classify(20) != 4 {
        return 4;
    }
    return 0;
}
