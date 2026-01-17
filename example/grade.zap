fun main() -> i32 {
    var score: i32 = 75;
    
    if score >= 90 {
        println("Grade: A");
    }
    else if score >= 80 {
        println("Grade: B");
    }
    else if score >= 70 {
        println("Grade: C");
    }
    else if score >= 60 {
        println("Grade: D");
    }
    else {
        println("Grade: F");
    }
    
    return 0;
}
