fun main() Int {
    var sum: Int = 0;
    var flag: Bool = true;

    if flag {
        sum = sum + 10;
    } else {
        sum = sum + 1;
    }

    if false {
        sum = sum + 100;
    } else {
        sum = sum + 20;
    }

    return sum - 30;
}
