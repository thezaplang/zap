fun main() Int{
    var i: Int = 0;
    var sum: Int = 0;
    
    while i < 10 {
        sum = sum + i;
        i = i + 1;
    }
    
    var result: Int = 0;
    if sum == 45 {
        result = 1;
    } else {
        result = 0;
    }
    return result;
}
