fun main() Int {
    var value: Int = 0;

    if true {
        if false {
            value = 1;
        } else {
            if true {
                value = 2;
            } else {
                value = 3;
            }
        }
    } else {
        value = 4;
    }

    return value - 2;
}
