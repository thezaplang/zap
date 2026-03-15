struct Person {
    name: String,
    age: Int,
    is_active: Bool,
    height: Float
}

fun main() {
    var p: Person = Person{
        name: "Alice",
        age: 30,
        is_active: true,
        height: 1.75
    };
    
    println(p.name);
    printInt(p.age);
    printBool(p.is_active);
    printFloat(p.height);
    
    p.name = "Bob";
    p.age = 35;
    p.is_active = false;
    p.height = 1.82;
    
    println(p.name);
    printInt(p.age);
    printBool(p.is_active);
    printFloat(p.height);
    
    return 0;
}
