fun decide(flag: Bool, alt: Bool) Int {
    if flag {
        return 1;
    }

    if alt {
        return 2;
    } else {
        return 3;
    }

    return 4;
}

fun main() Int {
    if decide(true, false) != 1 {
        return 1;
    }
    if decide(false, true) != 2 {
        return 2;
    }
    if decide(false, false) != 3 {
        return 3;
    }
    return 0;
}
