fun main() Int {
    var i: Int = 0;
    while (i < 10) {
        i = i + 1;
        if ((i % 2) == 0) {
            continue;
        }
    }
}
